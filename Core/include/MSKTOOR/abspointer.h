/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 3: absolute pointer protocol (touch / digitizers).
 * Complements pointer.h; kept separate because the structures differ and
 * most machines expose only one of the two protocols.
 */
#ifndef MKSTOOR_ABSPOINTER_H
#define MKSTOOR_ABSPOINTER_H

#include <mkstoor/types.h>
#include <mkstoor/efi.h>

typedef struct {
        u64 XMin;
        u64 YMin;
        u64 ZMin;
        u64 XMax;
        u64 YMax;
        u64 ZMax;
} EFI_ABSOLUTE_POINTER_MODE_MINMAX;

typedef struct {
        u64 CurrentX;
        u64 CurrentY;
        u64 CurrentZ;
        u32 Active;
        u32 LeftButton;   /* bitfield word in the real spec */
        u32 RightButton;
} EFI_ABSOLUTE_POINTER_STATE_FULL;

typedef struct EFI_ABSOLUTE_POINTER_PROTOCOL {
        EFI_STATUS (*Reset)(struct EFI_ABSOLUTE_POINTER_PROTOCOL *This,
                            BOOLEAN ExtendedVerification);
        EFI_STATUS (*GetState)(struct EFI_ABSOLUTE_POINTER_PROTOCOL *This,
                               EFI_ABSOLUTE_POINTER_STATE_FULL *State);
        void *WaitForInput;
        EFI_ABSOLUTE_POINTER_MODE_MINMAX *Mode; /* simplified: min/max block */
} EFI_ABSOLUTE_POINTER_PROTOCOL;

static const EFI_GUID mks_absolute_pointer_guid =
        MKS_GUID(0x8b5c8afa, 0x8b79, 0x476f,
               0x8a, 0x57, 0xd4, 0x17, 0x41, 0x82, 0xc2, 0x1f);

#endif /* MKSTOOR_ABSPOINTER_H */
