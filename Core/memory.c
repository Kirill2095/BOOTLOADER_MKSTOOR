/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — early bump heap + Page Frame Allocator (Stage 1).
 *
 * The PFA is a flat bitmap over physical frames, built from every
 * EfiConventionalMemory region in the harvested EFI memory map. Because we
 * run identity-mapped before ExitBootServices, "physical" == "virtual".
 *
 * Allocation policy: first-fit contiguous runs, preferring frames already
 * inside a single map region. Owner tags are recorded per-run for leak
 * accounting (design invariant #2).
 */
#include <mkstoor/memory.h>
#include <mkstoor/console.h>

#define MKS_PFA_MAX_FRAMES (512u * 1024u * 1024 / MKS_PAGE_SIZE) /* 512 GiB span cap */
#define MKS_OWNERS_MAX     32
#define MKS_MAP_COPY_MAX   64 /* descriptors retained */

/* ---------------- early bump heap ---------------- */
static u8  *g_early_base;
static usize g_early_size;
static usize g_early_off;

mkstoor_err_t mks_early_init(void *base, usize size)
{
        if (!base || size < 4096)
                return MKSTOOR_ERR_INVALID_ARG;
        g_early_base = base;
        g_early_size = size;
        g_early_off  = 0;
        return MKSTOOR_OK;
}

void *mks_early_alloc_aligned(usize size, usize align)
{
        if (!g_early_base || align == 0 || (align & (align - 1)))
                return NULL;
 usize off = ALIGN_UP(g_early_off, align);
        usize end = off + ALIGN_UP(size, 8);
        if (end > g_early_size)
                return NULL;
        g_early_off = end;
        return g_early_base + off;
}

void *mks_early_alloc(usize size)
{
        return mks_early_alloc_aligned(size, 8);
}

usize mks_early_used(void)
{
        return g_early_off;
}

/* ---------------- owner table ---------------- */
struct owner_rec {
        const char *name;
        u64 pages;
};
static struct owner_rec g_owners[MKS_OWNERS_MAX];
static usize g_nowners;

static int owner_lookup(const char *name, bool create)
{
        if (!name)
                name = "core";
        for (usize i = 0; i < g_nowners; i++)
                if (g_owners[i].name == name)
                        return (int)i;
        if (!create)
                return -1;
        if (g_nowners >= MKS_OWNERS_MAX)
                return -1;
        g_owners[g_nowners].name = name;
        g_owners[g_nowners].pages = 0;
        return (int)g_nowners++;
}

/* ---------------- PFA bitmap ---------------- */
static u8 *g_bitmap;               /* 1 bit per frame, 1 = free */
static u64 g_max_frame;            /* frames covered by bitmap  */
static u64 g_free_frames;
static u64 g_total_frames;         /* conventional memory total */
static u64 g_reserved_frames;      /* non-free accounted frames */

static inline void bm_set(u64 f)   { g_bitmap[f >> 3] |=  (u8)(1u << (f & 7)); }
static inline void bm_clear(u64 f) { g_bitmap[f >> 3] &= ~(u8)(1u << (f & 7)); }
static inline bool bm_test(u64 f)  { return g_bitmap[f >> 3] & (u8)(1u << (f & 7)); }

/* Retained memory map for ExitBootServices handover. */
static EFI_MEMORY_DESCRIPTOR g_map_copy[MKS_MAP_COPY_MAX];
static usize g_map_size, g_map_desc_size;
static u64   g_map_key;

mkstoor_err_t mks_pfa_init(EFI_SYSTEM_TABLE *st)
{
        EFI_BOOT_SERVICES_FULL *bs = st->BootServices;
        usize map_size = 0, desc_size = 0;
        u32 desc_ver = 0;
        EFI_MEMORY_DESCRIPTOR *map = NULL;

        /* Two-call idiom: probe size, then fetch with slack for growth. */
        EFI_STATUS e = bs->GetMemoryMap(&map_size, NULL, &g_map_key,
                                        &desc_size, &desc_ver);
        if (e != EFI_BUFFER_TOO_SMALL && EFI_ERROR(e))
                return MKSTOOR_ERR_IO;
        map_size += 2 * desc_size;
        map = mks_early_alloc_aligned(map_size, 8);
        if (!map)
                return MKSTOOR_ERR_NO_MEMORY;
        e = bs->GetMemoryMap(&map_size, map, &g_map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(e))
                return MKSTOOR_ERR_IO;

        usize n = map_size / desc_size;
        mks_info("mem", "memory map parsed: %u regions", (unsigned)n);

        /* Pass 1: total conventional frames -> size the bitmap. */
        u64 hi_frame = 0, total = 0;
        for (usize i = 0; i < n; i++) {
                EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)
                        ((u8 *)map + i * desc_size);
                if (d->Type == EfiConventionalMemory) {
                        total += d->NumberOfPages;
                        u64 end = d->PhysicalStart / MKS_PAGE_SIZE + d->NumberOfPages;
                        if (end > hi_frame)
                                hi_frame = end;
                }
        }
        if (hi_frame > MKS_PFA_MAX_FRAMES)
                hi_frame = MKS_PFA_MAX_FRAMES; /* top beyond cap stays reserved-by-absence */

        g_bitmap = mks_early_alloc_aligned((hi_frame + 7) / 8, 8);
        if (!g_bitmap)
                return MKSTOOR_ERR_NO_MEMORY;
        g_max_frame = hi_frame;
        memset(g_bitmap, 0, (hi_frame + 7) / 8);

        /* Pass 2: mark free frames. Everything else (boot services code/data,
         * runtime regions, MMIO) stays marked used implicitly (bit == 0). */
        for (usize i = 0; i < n; i++) {
                EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)
                        ((u8 *)map + i * desc_size);
                if (d->Type != EfiConventionalMemory)
                        continue;
                u64 start = d->PhysicalStart / MKS_PAGE_SIZE;
                u64 count = d->NumberOfPages;
                if (start + count > g_max_frame)
                        count = g_max_frame > start ? g_max_frame - start : 0;
                for (u64 f = start; f < start + count; f++) {
                        bm_set(f);
                        g_free_frames++;
                }
        }
        g_total_frames = total;
        g_reserved_frames = 0;

        /* Retain the map copy for handover. */
        if (n <= MKS_MAP_COPY_MAX) {
                memcpy(g_map_copy, map, map_size);
                g_map_size = map_size;
                g_map_desc_size = desc_size;
        } else {
                mks_warn("mem", "memory map too large (%u) to retain for handover",
                         (unsigned)n);
        }
        return MKSTOOR_OK;
}

void mks_pfa_dump_stats(void)
{
        mks_info("mem", "PFA online: TOTAL=%llu FREE=%llu USED=%llu RESERVED=%llu pages",
                 (unsigned long long)g_total_frames,
                 (unsigned long long)g_free_frames,
                 (unsigned long long)(g_total_frames - g_free_frames),
                 (unsigned long long)g_reserved_frames);
}

mkstoor_err_t mks_pfa_stats(struct mks_pfa_stats *out)
{
        if (!out)
                return MKSTOOR_ERR_INVALID_ARG;
        out->total_pages    = g_total_frames;
        out->free_pages     = g_free_frames;
        out->used_pages     = g_total_frames - g_free_frames;
        out->reserved_pages = g_reserved_frames;
        return MKSTOOR_OK;
}

/* First-fit contiguous run of `count` free frames. Returns start frame or
 * (u64)-1. Alignment in frames (1 = natural). */
static u64 pfa_find_run(u64 count, u64 align_frames)
{
        u64 run = 0;
        for (u64 f = 0; f < g_max_frame; f++) {
                if (bm_test(f)) {
                        run++;
                        if (run >= count &&
                            ((f + 1) % align_frames == 0 || align_frames == 1)) {
                                u64 end = f + 1;
                                return end - count;
                        }
                } else {
                        run = 0;
                }
        }
        return (u64)-1;
}

void *mks_frames_alloc(u64 count, const char *owner)
{
        if (!g_bitmap || count == 0)
                return NULL;
        u64 start = pfa_find_run(count, 1);
        if (start == (u64)-1)
                return NULL;
        for (u64 f = start; f < start + count; f++) {
                bm_clear(f);
                g_free_frames--;
        }
        int oi = owner_lookup(owner, true);
        if (oi >= 0)
                g_owners[oi].pages += count;
        return (void *)(start * MKS_PAGE_SIZE);
}

mkstoor_err_t mks_frames_reserve(u64 start_page, u64 count, const char *owner)
{
        if (!g_bitmap)
                return MKSTOOR_ERR_GENERIC;
        if (start_page + count > g_max_frame)
                return MKSTOOR_ERR_INVALID_ARG;
        for (u64 f = start_page; f < start_page + count; f++) {
                if (!bm_test(f))
                        return MKSTOOR_ERR_INVALID_ARG; /* already used */
                bm_clear(f);
                g_free_frames--;
                g_reserved_frames++;
        }
        int oi = owner_lookup(owner, true);
        if (oi >= 0)
                g_owners[oi].pages += count;
        return MKSTOOR_OK;
}

void mks_frames_free(void *vaddr, u64 count, const char *owner)
{
        u64 start = (u64)(uintptr_t)vaddr / MKS_PAGE_SIZE;
        if (!g_bitmap || start + count > g_max_frame)
                return;
        for (u64 f = start; f < start + count; f++) {
                if (!bm_test(f)) {
                        bm_set(f);
                        g_free_frames++;
                }
        }
        int oi = owner_lookup(owner, false);
        if (oi >= 0 && g_owners[oi].pages >= count)
                g_owners[oi].pages -= count;
}

u64 mks_module_bytes_owned(const char *name)
{
        int oi = owner_lookup(name, false);
        return oi >= 0 ? g_owners[oi].pages * MKS_PAGE_SIZE : 0;
}

mkstoor_err_t mks_memmap_get(EFI_MEMORY_DESCRIPTOR **map, usize *size,
                             usize *desc_size, u64 *map_key)
{
        if (!g_map_size)
                return MKSTOOR_ERR_NOT_FOUND;
        *map = g_map_copy;
        *size = g_map_size;
        *desc_size = g_map_desc_size;
        *map_key = g_map_key;
        return MKSTOOR_OK;
}
