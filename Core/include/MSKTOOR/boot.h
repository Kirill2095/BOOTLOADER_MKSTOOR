/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — Stage 2: Linux EFI-stub handover.
 *
 * Implements the boot protocol documented in
 * Documentation/arch/x86/boot.rst of the Linux kernel: we locate the
 * "EFI handover protocol" magic (osimage header at setup_header.handler_data
 * offset when handover_offset != 0), build a new PE/COFF-compatible load
 * into allocated frames, fill linux_kernel_params (zero page) and jump to
 * the kernel's efi_entry() handover with ExitBootServices already done.
 *
 * If the image lacks the handover protocol we fall back to legacy EFI stub
 * loading via BS->StartImage semantics is NOT possible after EBS, so v0.2
 * requires bzImages with CONFIG_EFI_HANDOVER_PROTOCOL (>= 6.9) or any
 * self-declaring PE EFI stub image loaded *before* EBS.
 */
#ifndef MKSTOOR_BOOT_H
#define MKSTOOR_BOOT_H

#include <mkstoor/types.h>
#include <mkstoor/efi.h>

struct mks_boot_ctx {
        EFI_SYSTEM_TABLE *st;
        EFI_HANDLE        image_handle;
        const char       *cmdline;
        /* initrd lives in allocated frames preserved across EBS */
        u64 initrd_addr;
        u64 initrd_size;
};

/* Boot a loaded Linux kernel image buffer. Never returns on success. */
MKSTOOR_API mkstoor_err_t mks_boot_linux(struct mks_boot_ctx *ctx,
                                         const void *kernel, usize klen);

/* Capability-vector trampoline used by modules holding MKS_MOD_EXEC. */
MKSTOOR_API mkstoor_err_t mks_exec_boot_kernel(const void *kernel, usize klen,
                                               const void *initrd, usize ilen,
                                               const char *cmdline);

/* Set once by efi_main before the pipeline runs. */
void mks_boot_set_context(EFI_SYSTEM_TABLE *st, EFI_HANDLE ih);

#endif /* MKSTOOR_BOOT_H */
