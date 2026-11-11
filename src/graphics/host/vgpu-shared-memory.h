/* Shared GPU backing: virtual contiguity, independently mapped physical pages.
 * Prefer local huge pages to reduce Gunyah metadata, but allow pinned base pages
 * when compaction fails. Qualify mapping capacity separately from allocation. */
#ifndef OPENOOS_VGPU_SHARED_MEMORY_H
#define OPENOOS_VGPU_SHARED_MEMORY_H
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif
#define VGPU_SHARED_FOLIO (2u * 1024u * 1024u)
#ifndef VGPU_SHARED_PREFER_HUGE
#define VGPU_SHARED_PREFER_HUGE 1
#endif
#define VGPU_PAGE_PRESENT (UINT64_C(1) << 63)
#define VGPU_PAGE_SWAPPED (UINT64_C(1) << 62)
#define VGPU_PAGE_PFN ((UINT64_C(1) << 55) - 1)
#define VGPU_PAGE_HEAD (UINT64_C(1) << 15)
#define VGPU_PAGE_TAIL (UINT64_C(1) << 16)
#define VGPU_PAGE_THP (UINT64_C(1) << 22)
#define VGPU_PAGE_SWAPBACKED (UINT64_C(1) << 14)

static inline int vgpu_shared_folio_valid(const uint64_t pages[512],
                                         const uint64_t flags[512]) {
    uint64_t first = pages[0] & VGPU_PAGE_PFN;
    const uint64_t mask = VGPU_PAGE_HEAD | VGPU_PAGE_TAIL | VGPU_PAGE_THP;
    /* kpageflags exports SWAPBACKED on the compound head. Tail flags
     * describe the individual page and need not repeat the head bit. */
    if (!first || first % 512 || !(flags[0] & VGPU_PAGE_SWAPBACKED)) return -1;
    for (size_t i = 0; i < 512; ++i) {
        uint64_t expected = VGPU_PAGE_THP |
                            (i ? VGPU_PAGE_TAIL : VGPU_PAGE_HEAD);
        if ((pages[i] & (VGPU_PAGE_PRESENT | VGPU_PAGE_SWAPPED)) != VGPU_PAGE_PRESENT ||
            (pages[i] & VGPU_PAGE_PFN) != first + i || (flags[i] & mask) != expected) return -1;
    }
    return 0;
}

/* A chunk may contain independent swap-backed base pages or complete THPs.
 * Never infer DMA addresses from adjacent virtual addresses. Validate compound
 * boundaries too: Gunyah pins and maps the complete containing folio. */
static inline int vgpu_shared_pages_valid(const uint64_t pages[512],
                                          const uint64_t flags[512],
                                          size_t *base_pages, size_t *large_folios) {
    const uint64_t mask = VGPU_PAGE_HEAD | VGPU_PAGE_TAIL | VGPU_PAGE_THP;
    size_t base = 0, large = 0;
    for (size_t i = 0; i < 512;) {
        uint64_t pfn = pages[i] & VGPU_PAGE_PFN;
        if (!pfn || (pages[i] & (VGPU_PAGE_PRESENT | VGPU_PAGE_SWAPPED)) != VGPU_PAGE_PRESENT ||
            !(flags[i] & VGPU_PAGE_SWAPBACKED)) return -1;
        if (!(flags[i] & mask)) { ++base; ++i; continue; }
        if ((flags[i] & mask) != (VGPU_PAGE_HEAD | VGPU_PAGE_THP)) return -1;
        size_t end = i + 1;
        while (end < 512 && (flags[end] & mask) == (VGPU_PAGE_TAIL | VGPU_PAGE_THP)) {
            if ((pages[end] & (VGPU_PAGE_PRESENT | VGPU_PAGE_SWAPPED)) != VGPU_PAGE_PRESENT ||
                (pages[end] & VGPU_PAGE_PFN) != pfn + end - i) return -1;
            ++end;
        }
        size_t count = end - i;
        if (count < 2 || (count & (count - 1)) || pfn % count || i % count) return -1;
        ++large;
        i = end;
    }
    *base_pages += base;
    *large_folios += large;
    return 0;
}

static inline int vgpu_shared_verify(void *mapping, size_t size) {
    if (mapping == MAP_FAILED || !size || size % VGPU_SHARED_FOLIO ||
        (uintptr_t)mapping % VGPU_SHARED_FOLIO) { errno = EINVAL; return -1; }
    int maps = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    int flags_fd = open("/proc/kpageflags", O_RDONLY | O_CLOEXEC);
    int result = -1;
    size_t base_pages = 0, large_folios = 0, physical_runs = 0;
    if (maps < 0 || flags_fd < 0) goto out;
    for (size_t offset = 0; offset < size; offset += VGPU_SHARED_FOLIO) {
        uint64_t pages[512], flags[512];
        off_t index = (off_t)(((uintptr_t)mapping + offset) / 4096 * 8);
        if (pread(maps, pages, sizeof(pages), index) != sizeof(pages)) goto out;
        /* Reject missing/swapped/hidden PFNs before using them as file offsets. */
        for (size_t i = 0; i < 512; ++i) {
            if ((pages[i] & (VGPU_PAGE_PRESENT | VGPU_PAGE_SWAPPED)) != VGPU_PAGE_PRESENT ||
                !(pages[i] & VGPU_PAGE_PFN)) {
                errno = ENOMEM; goto invalid;
            }
        }
        /* Coalesce reads only where PFNs really are adjacent. */
        for (size_t i = 0; i < 512;) {
            uint64_t first = pages[i] & VGPU_PAGE_PFN;
            size_t end = i + 1;
            while (end < 512 && (pages[end] & VGPU_PAGE_PFN) == first + end - i) ++end;
            size_t bytes = (end - i) * sizeof(flags[0]);
            if (pread(flags_fd, flags + i, bytes, (off_t)(first * 8)) != (ssize_t)bytes) goto out;
            ++physical_runs;
            i = end;
        }
        if (vgpu_shared_pages_valid(pages, flags, &base_pages, &large_folios)) {
            errno = ENOMEM; goto invalid;
        }
        continue;
invalid:
        fprintf(stderr, "VGPU_SHARED_FOLIO_INVALID offset=%zu\n", offset);
        goto out;
    }
    printf("VGPU_SHARED_PAGES bytes=%zu base_pages=%zu large_folios=%zu physical_runs=%zu verified=1\n",
           size, base_pages, large_folios, physical_runs);
    result = 0;
out:;
    int saved = errno;
    if (maps >= 0) close(maps);
    if (flags_fd >= 0) close(flags_fd);
    errno = saved;
    return result;
}

static inline void *vgpu_shared_prepare(int fd, size_t size, int pin) {
    if (!size || size % VGPU_SHARED_FOLIO || size > SIZE_MAX - VGPU_SHARED_FOLIO) {
        errno = EINVAL; return MAP_FAILED;
    }
    size_t reserved = size + VGPU_SHARED_FOLIO;
    void *range = mmap(NULL, reserved, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (range == MAP_FAILED) return MAP_FAILED;
    uintptr_t aligned = ((uintptr_t)range + VGPU_SHARED_FOLIO - 1) &
                        ~((uintptr_t)VGPU_SHARED_FOLIO - 1);
    void *mapping = mmap((void *)aligned, size, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_FIXED, fd, 0);
    if (mapping == MAP_FAILED) {
        int saved = errno; munmap(range, reserved); errno = saved; return MAP_FAILED;
    }
    size_t prefix = aligned - (uintptr_t)range;
    if (prefix) munmap(range, prefix);
    size_t suffix = reserved - prefix - size;
    if (suffix) munmap((void *)(aligned + size), suffix);
    if (madvise(mapping, size, VGPU_SHARED_PREFER_HUGE ? MADV_HUGEPAGE : MADV_NOHUGEPAGE) ||
        madvise(mapping, size, MADV_DONTDUMP)) goto fail;
    size_t collapse_failures = 0;
    for (size_t offset = 0; offset < size; offset += VGPU_SHARED_FOLIO) {
        void *chunk = (char *)mapping + offset;
        memset(chunk, 0, VGPU_SHARED_FOLIO);
        if (VGPU_SHARED_PREFER_HUGE && !collapse_failures && madvise(chunk, VGPU_SHARED_FOLIO, MADV_COLLAPSE)) {
            /* The existing shmem pages remain valid. Residency/folio validation
             * below is mandatory; this only relaxes the allocation shape. */
            if (!collapse_failures)
                fprintf(stderr, "VGPU_SHARED_BASE_FALLBACK first_offset=%zu collapse_errno=%d\n", offset, errno);
            ++collapse_failures;
            /* Do not repeat expensive compaction for every remaining chunk. */
            if (madvise(chunk, size - offset, MADV_NOHUGEPAGE)) goto fail;
        }
    }
    if ((pin && mlock(mapping, size)) || madvise(mapping, size, MADV_NOHUGEPAGE) ||
        vgpu_shared_verify(mapping, size)) goto fail;
    printf("VGPU_SHARED_ALLOCATION collapse_failures=%zu locked=%d\n", collapse_failures, !!pin);
    return mapping;
fail:;
    int saved = errno;
    munmap(mapping, size);
    errno = saved;
    return MAP_FAILED;
}
#endif
