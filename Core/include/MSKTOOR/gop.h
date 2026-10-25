/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 3: UEFI Graphics Output Protocol wrapper.
 *
 * The GOP gives us a linear framebuffer in one of several pixel formats;
 * this layer normalises every write to ARGB32 (X8B8G8R8) so the renderer,
 * theme engine and font blitter never branch on the native format. On
 * hardware with BGR ordering or byte-shifted channels the conversion cost
 * is paid once per pixel in mks_gop_put() — measured acceptable for a
 * boot menu that repaints at most a few times per second.
 *
 * All framebuffer memory belongs to the firmware; we only ever write into
 * the LFB the GOP hands us and restore it via Blt(VideoOnly) before
 * handing over to the kernel (docs/ARCHITECTURE.md §gop).
 */
#ifndef MKSTOOR_GOP_H
#define MKSTOOR_GOP_H

#include <mkstoor/types.h>
#include <mkstoor/efi.h>

/* Packed ARGB32 colour: 0xRRGGBB built from the theme, stored as X8B8G8R8. */
typedef u32 mks_rgb_t;

static inline mks_rgb_t mks_rgb(u8 r, u8 g, u8 b)
{
        return ((mks_rgb_t)r << 16) | ((mks_rgb_t)g << 8) | (mks_rgb_t)b;
}

static inline u8 mks_rgb_r(mks_rgb_t c) { return (u8)(c >> 16); }
static inline u8 mks_rgb_g(mks_rgb_t c) { return (u8)(c >> 8); }
static inline u8 mks_rgb_b(mks_rgb_t c) { return (u8)c; }

struct mks_gop_info {
        u32 width;            /* usable pixels (Mode information)          */
        u32 height;
        u32 pitch;            /* bytes per framebuffer line                */
        void *fb;             /* linear framebuffer base                   */
        EFI_GRAPHICS_PIXEL_FORMAT fmt; /* native format, for diagnostics   */
};

/* Locate GOP on ConsoleOutHandle (falling back to LocateProtocol) and
 * program the mode whose resolution best matches `want_w` x `want_h`.
 * Safe to call when no GOP exists: returns MKSTOOR_ERR_NOT_FOUND and the
 * pipeline then keeps the text console. */
MKSTOOR_API mkstoor_err_t mks_gop_init(EFI_SYSTEM_TABLE *st,
                                       u32 want_w, u32 want_h,
                                       struct mks_gop_info *out_info);

/* Shutdown helpers (idempotent). */
MKSTOOR_API bool mks_gop_available(void);
MKSTOOR_API const struct mks_gop_info *mks_gop_get(void);
MKSTOOR_API void mks_gop_clear(mks_rgb_t color);
MKSTOOR_API void mks_gop_fill_rect(u32 x, u32 y, u32 w, u32 h, mks_rgb_t c);
MKSTOOR_API void mks_gop_blend_rect(u32 x, u32 y, u32 w, u32 h,
                                    mks_rgb_t c, u8 alpha /* 0..255 */);
MKSTOOR_API void mks_gop_put(u32 x, u32 y, mks_rgb_t c);
MKSTOOR_API void mks_gop_hline(u32 x, u32 y, u32 w, mks_rgb_t c);
MKSTOOR_API void mks_gop_vline(u32 x, u32 y, u32 h, mks_rgb_t c);
MKSTOOR_API void mks_gop_frame(u32 x, u32 y, u32 w, u32 h, mks_rgb_t c);

/* Copy an RGBA8888 source image (e.g. a decoded logo) onto the LFB,
 * honouring its alpha channel. Bounds-checked against both buffers. */
MKSTOOR_API void mks_gop_blit_rgba(u32 x, u32 y, u32 w, u32 h,
                                   const u8 *src, u32 src_pitch);

#endif /* MKSTOOR_GOP_H */
