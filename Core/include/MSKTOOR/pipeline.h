/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 2 boot pipeline (core/pipeline.c).
 *
 * The pipeline is the orchestrator between efi_main() and the kernel
 * handover in core/boot.c. It owns: block-device enumeration, FAT/ext4
 * mounting, config discovery + TOML parsing, menu construction, image
 * reading, measurement hooks and final entry selection.
 */
#ifndef MKSTOOR_PIPELINE_H
#define MKSTOOR_PIPELINE_H

#include <mkstoor/types.h>
#include <mkstoor/efi.h>

/* Run the whole Stage-2 flow. Returns an error only when nothing could be
 * booted; on a successful kernel handover it never returns. */
MKSTOOR_API mkstoor_err_t mks_stage2_run(EFI_SYSTEM_TABLE *st, EFI_HANDLE ih);

/* Publish/lookup the firmware table for renderer modules that need input
 * services but must not re-acquire them (core/ui/menu_gop.c). */
MKSTOOR_API void mks_pipeline_set_context(EFI_SYSTEM_TABLE *st, EFI_HANDLE ih);
EFI_SYSTEM_TABLE *mks_pipeline_efi_table(void);

#endif /* MKSTOOR_PIPELINE_H */
