/*
 * Mesa 3-D graphics library
 *
 * Copyright © 2025, Google Inc.
 * SPDX-License-Identifier: MIT
 */

#include <hardware/gralloc.h>
#include <string.h>

#include "drm-uapi/drm_fourcc.h"
#include "util/macros.h"
#include "u_gralloc_qcom_native_handle.h"
#include "u_gralloc_internal.h"

/*
 * Qualcomm grallocs describe the buffer layout only through their private
 * handle, so the modifier has to be recovered from it.
 *
 * The QTI display gralloc (hardware/qcom/display private_handle_t, used up to
 * and including the gralloc4 QtiMapper) begins the int payload with a magic
 * followed by the flags word:
 *
 *    int magic;   // 'gmsm'
 *    int flags;   // PRIV_FLAGS_*
 *
 * Snapdragon 8 Elite generation devices ship SnapAlloc instead
 * (vendor/qcom/opensource/display-core), whose handle has no magic.  It is a
 * C++ object holding one {fd, fd_metadata} pair and one properties block per
 * view, a single view unless the buffer was allocated as multiview:
 *
 *    template <int N>
 *    class SnapHandleData : public SnapHandle {          // a native_handle_t
 *       std::array<FdPair, N> fdPairArray;
 *       std::array<SnapHandleProperties, N> propertiesArray;
 *    };
 *
 * SnapHandleProperties has 64-bit members and the fd pairs always leave the
 * payload 4 byte aligned, so the properties start one int past the last fd.
 * That padding int is not counted in numInts, which is exactly the size of the
 * properties, and is what identifies the layout here: a revision with a
 * property size we do not support is simply not recognised.
 *
 * Nothing in a SnapAlloc handle names its owner, so also require the
 * properties to describe the buffer we were handed.  SnapAlloc returns
 * aligned_width_in_pixels as the stride of the allocation, which is the value
 * that ends up in hnd->pixel_stride.
 */

/* PRIV_FLAGS_UBWC_ALIGNED, this UBWC flag was introduced in a5xx. */
#define QCOM_PRIV_FLAGS_UBWC_ALIGNED 0x08000000

/* PRIV_VIEW_MASK_PRIMARY, the view every SnapAlloc handle starts with. */
#define SNAPALLOC_VIEW_PRIMARY 0x1

/* sizeof(SnapHandleProperties) / sizeof(int), see above.
 * 136B (34 ints) on 8 Elite (display-core/SnapAlloc),
 * 132B (33 ints) on parrot/SM7435 QTI gralloccore (snapalloc enabled).
 * 152B (38 ints) on Retroid Pocket Duo (Adreno 722, Android 16).
 */
#define SNAPALLOC_INTS_PER_VIEW 34
#define SNAPALLOC_INTS_PER_VIEW_PARROT 33
#define SNAPALLOC_INTS_PER_VIEW_DUO 38

/* The beginning of SnapHandleProperties, the rest is not needed here. */
struct snapalloc_properties {
   uint32_t view;
   int flags;
   int aligned_width_in_bytes;
   int aligned_width_in_pixels;
};

static bool
snapalloc_check_props(const struct snapalloc_properties *props,
                      const struct u_gralloc_buffer_handle *hnd)
{
   return props->view == SNAPALLOC_VIEW_PRIMARY &&
          props->aligned_width_in_pixels == hnd->pixel_stride;
}

static bool
get_snapalloc_priv_flags(const native_handle_t *handle, const int *ints,
                         int per_view,
                         struct u_gralloc_buffer_handle *hnd, int *out_flags)
{
   struct snapalloc_properties props;

   if (handle->numInts < per_view)
      return false;

   memcpy(&props, ints + 1, sizeof(props));

   if (!snapalloc_check_props(&props, hnd))
      return false;

   /* The Duo capture reports both strides in the same padded header. */
   if (per_view == SNAPALLOC_INTS_PER_VIEW_DUO) {
      int bpp = get_hal_format_bpp(hnd->hal_format);

      if (bpp == 0 || props.aligned_width_in_pixels <= 0 ||
          props.aligned_width_in_bytes !=
             (int64_t)props.aligned_width_in_pixels * bpp)
         return false;
   }

   *out_flags = props.flags;
   return true;
}

/*
 * Another revision with the same 34 ints per view, seen on an OPPO CPH2671
 * (SM8750 sun, Adreno 830, Android API 37), has no padding int: the
 * properties start at int 0 with the view id 0, and the int after the flags
 * is the stride in pixels instead of the stride in bytes:
 *
 *    ints[0]       view (0)
 *    ints[1]       flags (PRIV_FLAGS_*)
 *    ints[2]       aligned width in pixels (== pixel_stride)
 *    ints[3]       aligned height
 *    ints[4..5]    width, height
 *    ints[13..14]  size of the allocation in bytes (64 bit)
 *
 * Accept this layout only when the stride matches and the size of the
 * allocation agrees with the flags: the aligned image, optionally rounded up
 * to 4 KiB, for a linear buffer, a little more for the UBWC metadata of a
 * compressed one.
 */
#define SNAPALLOC_UNPADDED_SIZE_INT 13

static bool
get_snapalloc_priv_flags_unpadded(const native_handle_t *handle,
                                  const int *ints,
                                  struct u_gralloc_buffer_handle *hnd,
                                  int *out_flags)
{
   if (handle->numInts < SNAPALLOC_INTS_PER_VIEW)
      return false;

   uint32_t view = ints[0];
   int flags = ints[1];
   int aligned_width_in_pixels = ints[2];
   int aligned_height = ints[3];
   int bpp = get_hal_format_bpp(hnd->hal_format);

   if ((view != 0 && view != SNAPALLOC_VIEW_PRIMARY) || bpp == 0 ||
       aligned_width_in_pixels != hnd->pixel_stride || aligned_height <= 0)
      return false;

   uint64_t size;
   memcpy(&size, ints + SNAPALLOC_UNPADDED_SIZE_INT, sizeof(size));

   uint64_t image_size =
      (uint64_t)aligned_width_in_pixels * aligned_height * bpp;
   /* SnapAlloc rounds the final allocation size to its 4 KiB PAGE_SIZE. */
   uint64_t aligned_size = ALIGN_POT(image_size, UINT64_C(4096));
   bool ubwc = flags & QCOM_PRIV_FLAGS_UBWC_ALIGNED;
   if (ubwc ? (size <= image_size || size > image_size + image_size / 4)
            : (size != image_size && size != aligned_size))
      return false;

   *out_flags = flags;
   return true;
}

/*
 * The SM7435 (parrot) QTI gralloccore with snapalloc enabled packs the
 * properties one int earlier than the 8 Elite display-core build and may
 * report the primary view id as 0, so try both offsets and accept either
 * view id.  The stride match is the primary signal; aligned_width_in_bytes
 * is checked against pixel_stride * bpp when the HAL format bpp is known,
 * but a mismatch alone does not reject the handle (the parrot revision
 * does not pack the fields exactly like display-core).
 *
 * As a last resort, scan for the (aligned_bytes, aligned_pixels) pair:
 * on the observed parrot handle the bytes/pixels sit at ints[1]/ints[2]
 * while the struct overlay misses them, so without the scan that handle
 * falls back to INVALID.  The int right before the pair is taken as the
 * flags word.
 */
static bool
get_snapalloc_priv_flags_parrot(const native_handle_t *handle, const int *ints,
                                struct u_gralloc_buffer_handle *hnd,
                                int *out_flags)
{
   for (int off = 0; off <= 1; off++) {
      struct snapalloc_properties props;

      if (off + 4 > handle->numInts)
         continue;

      memcpy(&props, ints + off, sizeof(props));

      /* Check stride match first, view second (parrot uses 0). */
      if (props.aligned_width_in_pixels == hnd->pixel_stride &&
          (props.view == SNAPALLOC_VIEW_PRIMARY || props.view == 0)) {
         /* Also sanity-check aligned_bytes == stride * bpp if we can. */
         int bpp = get_hal_format_bpp(hnd->hal_format);
         if (bpp && props.aligned_width_in_bytes == hnd->pixel_stride * bpp) {
            *out_flags = props.flags;
            return true;
         }
         /* Even without bpp match, accept stride match. */
         *out_flags = props.flags;
         return true;
      }
   }

   /* Fallback: scan for stride pattern (handles future shifts without
    * hardcoding off). */
   int bpp = get_hal_format_bpp(hnd->hal_format);
   if (bpp) {
      int exp_bytes = hnd->pixel_stride * bpp;
      for (int i = 0; i + 1 < handle->numInts; i++) {
         if (ints[i] == exp_bytes && ints[i + 1] == hnd->pixel_stride) {
            /* Flags is one int before aligned_bytes. */
            if (i >= 1) {
               *out_flags = ints[i - 1];
               return true;
            }
         }
      }
   }

   return false;
}

static bool
get_priv_flags(struct u_gralloc_buffer_handle *hnd, int *out_flags)
{
   const native_handle_t *handle = hnd->handle;
   const int *ints = &handle->data[handle->numFds];
   const int gmsm = ('g' << 24) | ('m' << 16) | ('s' << 8) | 'm';

   if (handle->numInts >= 2 && ints[0] == gmsm) {
      *out_flags = ints[1];
      return true;
   }

   if (handle->numFds >= 2 && handle->numFds % 2 == 0) {
      int per_view = -1;

      if (handle->numInts == (handle->numFds / 2) * SNAPALLOC_INTS_PER_VIEW)
         per_view = SNAPALLOC_INTS_PER_VIEW;
      else if (handle->numInts ==
               (handle->numFds / 2) * SNAPALLOC_INTS_PER_VIEW_PARROT)
         per_view = SNAPALLOC_INTS_PER_VIEW_PARROT;
      else if (handle->numInts ==
               (handle->numFds / 2) * SNAPALLOC_INTS_PER_VIEW_DUO)
         per_view = SNAPALLOC_INTS_PER_VIEW_DUO;

      if (per_view == SNAPALLOC_INTS_PER_VIEW_DUO)
         return get_snapalloc_priv_flags(handle, ints, per_view, hnd, out_flags);
      if (per_view == SNAPALLOC_INTS_PER_VIEW)
         return get_snapalloc_priv_flags(handle, ints, per_view, hnd,
                                         out_flags) ||
                get_snapalloc_priv_flags_unpadded(handle, ints, hnd,
                                                  out_flags);
      if (per_view == SNAPALLOC_INTS_PER_VIEW_PARROT)
         return get_snapalloc_priv_flags_parrot(handle, ints, hnd, out_flags);
   }

   return false;
}

bool
u_gralloc_has_supported_qcom_native_handle(struct u_gralloc_buffer_handle *hnd)
{
   int priv_flags;

   return get_priv_flags(hnd, &priv_flags);
}

void
u_gralloc_qcom_native_handle_apply_modifier(struct u_gralloc_buffer_handle *hnd,
                                            struct u_gralloc_buffer_basic_info *out)
{
   int priv_flags;

   if (get_priv_flags(hnd, &priv_flags)) {
      bool ubwc = priv_flags & QCOM_PRIV_FLAGS_UBWC_ALIGNED;
      out->modifier = ubwc ? DRM_FORMAT_MOD_QCOM_COMPRESSED : DRM_FORMAT_MOD_LINEAR;
   }
}
