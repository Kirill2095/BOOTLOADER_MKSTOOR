/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 3: pointing devices (mouse & touch).
 *
 * UEFI exposes two pointer protocols:
 *   * EFI_SIMPLE_POINTER_PROTOCOL  — relative deltas (PS/2-style mice);
 *   * EFI_ABSOLUTE_POINTER_PROTOCOL — absolute coordinates normalised to
 *     a virtual 0..AbsoluteMaxX/Y box (touchscreens, digitizers).
 * We consume both and merge them into one logical cursor: absolute events
 * position the cursor directly, relative events integrate deltas scaled by
 * ResolutionX/Y (which encode units per pixel for that protocol).
 *
 * Device-path mapping: firmware attaches Simple Pointer to the *parent*
 * controller handle of its HID device path, so a naive LocateProtocol can
 * miss it on some stacks; we therefore enumerate all handles carrying the
 * protocol GUID and ConnectController each one once before use.
 */
#ifndef MKSTOOR_POINTER_H
#define MKSTOOR_POINTER_H

#include <mkstoor/types.h>
#include <mkstoor/efi.h>

struct mks_pointer_event {
        i32 x, y;             /* screen coordinates after this event */
        bool pressed;         /* left button / touch contact          */
        bool released;        /* left button up / contact lifted      */
        bool moved;           /* coordinates changed                  */
};

/* Discover pointers attached to `gop_handle`'s system (all devices are
 * accepted; firmware already filtered what reaches ConOut pairing).
 * Returns MKSTOOR_OK if at least one device was bound, NOT_FOUND
 * otherwise — callers must degrade gracefully to keyboard-only input. */
MKSTOOR_API mkstoor_err_t mks_pointer_init(EFI_SYSTEM_TABLE *st);
MKSTOOR_API bool mks_pointer_available(void);

/* Poll every bound device once (non-blocking). Fills `out` when an event
 * occurred. Coordinates are clamped to the framebuffer size captured at
 * mks_gop_init() time. */
MKSTOOR_API bool mks_pointer_poll(struct mks_pointer_event *out);

/* Current logical cursor position (for drawing the pointer sprite). */
MKSTOOR_API void mks_pointer_pos(i32 *x, i32 *y);
MKSTOOR_API void mks_pointer_set_pos(i32 x, i32 y);

#endif /* MKSTOOR_POINTER_H */
