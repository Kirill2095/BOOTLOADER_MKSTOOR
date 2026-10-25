/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — memory management: early bump heap + page frame allocator.
 * Stage 1 implements both; the PFA is the accounting layer every module
 * allocation flows through (design philosophy #1: per-owner tracking).
 */
#ifndef MKSTOOR_MEMORY_H
#define MKSTOOR_MEMORY_H

#include <mkstoor/types.h>
#include <mkstoor/efi.h>

#define MKS_PAGE_SIZE 4096u

/* ------------------------------------------------------------------------ */
/* Early bump allocator (pre-PFA bootstrap)                                  */
/* ------------------------------------------------------------------------ */
MKSTOOR_API mkstoor_err_t mks_early_init(void *base, usize size);
MKSTOOR_API void *mks_early_alloc(usize size);      /* 8-byte aligned   */
MKSTOOR_API void *mks_early_alloc_aligned(usize size, usize align);
MKSTOOR_API usize mks_early_used(void);

/* ------------------------------------------------------------------------ */
/* Page Frame Allocator                                                      */
/* ------------------------------------------------------------------------ */
struct mks_pfa_stats {
        u64 total_pages;
        u64 free_pages;
        u64 reserved_pages;
        u64 used_pages;
};

/* Harvest the EFI memory map and bring the PFA online over all
 * EfiConventionalMemory regions. Retains a copy of the map for later
 * ExitBootServices(). */
MKSTOOR_API mkstoor_err_t mks_pfa_init(EFI_SYSTEM_TABLE *st);
MKSTOOR_API void          mks_pfa_dump_stats(void);
MKSTOOR_API mkstoor_err_t mks_pfa_stats(struct mks_pfa_stats *out);

/* Allocate/free contiguous frames. `owner` tags the allocation for
 * per-module accounting (NULL == core). */
MKSTOOR_API void *mks_frames_alloc(u64 count, const char *owner);
MKSTOOR_API mkstoor_err_t mks_frames_reserve(u64 start_page, u64 count,
                                             const char *owner);
MKSTOOR_API void  mks_frames_free(void *vaddr, u64 count, const char *owner);

/* Snapshot of the last GetMemoryMap() (for handover). */
MKSTOOR_API mkstoor_err_t mks_memmap_get(EFI_MEMORY_DESCRIPTOR **map,
                                         usize *size, usize *desc_size,
                                         u64 *map_key);

#endif /* MKSTOOR_MEMORY_H */
