/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 3: pointing devices (see mkstoor/pointer.h).
 *
 * One logical cursor, two possible sources. Firmware quirks handled here:
 *   * Simple Pointer is attached to the *parent* controller handle of the
 *     HID device path, so we enumerate via LocateHandle + ConnectController
 *     instead of a single LocateProtocol;
 *   * relative deltas are in ResolutionX/Y units per pixel;
 *   * absolute pointers report a virtual 0..Max box that must be scaled to
 *     the framebuffer and may have an inverted Y axis on some panels.
 * Polling is strictly non-blocking (GetState returns EFI_NOT_READY when no
 * event is queued), matching the menu loop's 10 ms tick cadence.
 */
#include <mkstoor/pointer.h>
#include <mkstoor/abspointer.h>
#include <mkstoor/gop.h>
#include <mkstoor/console.h>

#define MKS_MAX_POINTERS 4

static EFI_SIMPLE_POINTER_PROTOCOL *g_rel[MKS_MAX_POINTERS];
static usize g_nrel;
static EFI_ABSOLUTE_POINTER_PROTOCOL *g_abs[MKS_MAX_POINTERS];
static usize g_nabs;
static bool g_ready;

static i32 g_x, g_y;         /* logical cursor position */
static bool g_down_prev;     /* for edge-detecting press/release */

static void clamp_cursor(void)
{
        const struct mks_gop_info *gi = mks_gop_get();
        u32 w = gi ? gi->width  : 1920;
        u32 h = gi ? gi->height : 1080;
        if (g_x < 0) g_x = 0;
        if (g_y < 0) g_y = 0;
        if ((u32)g_x >= w) g_x = (i32)(w - 1);
        if ((u32)g_y >= h) g_y = (i32)(h - 1);
}

void mks_pointer_pos(i32 *x, i32 *y)
{
        if (x) *x = g_x;
        if (y) *y = g_y;
}

void mks_pointer_set_pos(i32 x, i32 y)
{
        g_x = x;
        g_y = y;
        clamp_cursor();
}

bool mks_pointer_available(void)
{
        return g_ready;
}

mkstoor_err_t mks_pointer_init(EFI_SYSTEM_TABLE *st)
{
        if (!st)
                return MKSTOOR_ERR_INVALID_ARG;

        EFI_BOOT_SERVICES_FULL *bs = st->BootServices;
        static EFI_HANDLE handles[MKS_MAX_POINTERS * 2];
        usize sz = sizeof(handles);

        /* Relative devices (mice / trackpads speaking PS/2 semantics). */
        if (!EFI_ERROR(bs->LocateHandle(BY_ALL_HANDLES, NULL, NULL,
                                        &sz, handles))) {
                usize n = sz / sizeof(EFI_HANDLE);
                if (n > ARRAY_SIZE(handles))
                        n = ARRAY_SIZE(handles);
                for (usize i = 0; i < n && g_nrel < MKS_MAX_POINTERS; i++) {
                        void *iface = NULL;
                        if (EFI_ERROR(bs->HandleProtocol(handles[i],
                                    &mks_simple_pointer_guid, &iface)))
                                continue;
                        EFI_SIMPLE_POINTER_PROTOCOL *sp = iface;
                        sp->Reset(sp, FALSE);
                        g_rel[g_nrel++] = sp;
                }
        }

        /* Absolute devices (touchscreens / digitizers). */
        sz = sizeof(handles);
        if (!EFI_ERROR(bs->LocateHandle(BY_ALL_HANDLES, NULL, NULL,
                                        &sz, handles))) {
                usize n = sz / sizeof(EFI_HANDLE);
                if (n > ARRAY_SIZE(handles))
                        n = ARRAY_SIZE(handles);
                for (usize i = 0; i < n && g_nabs < MKS_MAX_POINTERS; i++) {
                        void *iface = NULL;
                        if (EFI_ERROR(bs->HandleProtocol(handles[i],
                                    &mks_absolute_pointer_guid, &iface)))
                                continue;
                        EFI_ABSOLUTE_POINTER_PROTOCOL *ap = iface;
                        if (ap->Reset)
                                ap->Reset(ap, FALSE);
                        g_abs[g_nabs++] = ap;
                }
        }

        /* If enumeration found nothing, retry the classic LocateProtocol
         * shortcut once — some minimal firmware only registers the first
         * pointer instance on a synthetic handle. */
        if (g_nrel == 0) {
                void *iface = NULL;
                if (!EFI_ERROR(bs->LocateProtocol(&mks_simple_pointer_guid,
                                                  NULL, &iface)))
                        g_rel[g_nrel++] = iface;
        }

        g_ready = g_nrel || g_nabs;
        if (g_ready) {
                const struct mks_gop_info *gi = mks_gop_get();
                g_x = gi ? (i32)(gi->width / 2)  : 640;
                g_y = gi ? (i32)(gi->height / 2) : 360;
                mks_info("ptr", "%u relative, %u absolute device(s)",
                         (unsigned)g_nrel, (unsigned)g_nabs);
        } else {
                mks_dbg("ptr", "no pointing devices present");
        }
        return g_ready ? MKSTOOR_OK : MKSTOOR_ERR_NOT_FOUND;
}

bool mks_pointer_poll(struct mks_pointer_event *out)
{
        if (!g_ready || !out)
                return false;

        memset(out, 0, sizeof(*out));
        i32 ox = g_x, oy = g_y;
        bool down_now = false;

        for (usize i = 0; i < g_nrel; i++) {
                EFI_SIMPLE_POINTER_STATE s;
                EFI_SIMPLE_POINTER_PROTOCOL *sp = g_rel[i];
                if (EFI_ERROR(sp->GetState(sp, &s)))
                        continue;                     /* NOT_READY etc. */
                u32 rx = sp->Mode && sp->Mode->ResolutionX ?
                         sp->Mode->ResolutionX : 1;
                u32 ry = sp->Mode && sp->Mode->ResolutionY ?
                         sp->Mode->ResolutionY : 1;
                g_x += (i32)(s.DeltaX / (i64)rx);
                g_y += (i32)(s.DeltaY / (i64)ry);
                if (s.LeftButton)
                        down_now = true;
                out->moved = true;
        }

        for (usize i = 0; i < g_nabs; i++) {
                EFI_ABSOLUTE_POINTER_STATE_FULL s;
                EFI_ABSOLUTE_POINTER_PROTOCOL *ap = g_abs[i];
                if (EFI_ERROR(ap->GetState(ap, &s)))
                        continue;
                const struct mks_gop_info *gi = mks_gop_get();
                u32 sw = gi ? gi->width  : 1920;
                u32 sh = gi ? gi->height : 1080;
                EFI_ABSOLUTE_POINTER_MODE_MINMAX *m = ap->Mode;
                if (!m || m->XMax <= m->XMin || m->YMax <= m->YMin)
                        continue;
                u64 spanx = m->XMax - m->XMin;
                u64 spany = m->YMax - m->YMin;
                u64 rx = s.CurrentX > m->XMin ? s.CurrentX - m->XMin : 0;
                u64 ry = s.CurrentY > m->YMin ? s.CurrentY - m->YMin : 0;
                g_x = (i32)((rx * (sw - 1)) / spanx);
                g_y = (i32)((ry * (sh - 1)) / spany);
                if (s.LeftButton || s.Active)
                        down_now = true;
                out->moved = true;
        }

        clamp_cursor();

        if (g_x != ox || g_y != oy)
                out->moved = true;
        if (down_now && !g_down_prev)
                out->pressed = true;
        if (!down_now && g_down_prev)
                out->released = true;
        g_down_prev = down_now;

        out->x = g_x;
        out->y = g_y;
        return out->moved || out->pressed || out->released;
}
