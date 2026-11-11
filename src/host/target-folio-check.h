/* Read-only verification of actual compound anonymous host pages.
 * smaps AnonHugePages only accounts for PMD THPs, not smaller PTE THPs. */
#ifndef TARGET_FOLIO_CHECK_H
#define TARGET_FOLIO_CHECK_H
#if TARGET_MIN_FOLIO_SIZE < 2 * 1024 * 1024
#define TARGET_PFN_MASK ((UINT64_C(1) << 55) - 1)
#define TARGET_PRESENT (UINT64_C(1) << 63)
#define TARGET_SWAPPED (UINT64_C(1) << 62)
#define TARGET_HEAD (UINT64_C(1) << 15)
#define TARGET_TAIL (UINT64_C(1) << 16)
#define TARGET_THP (UINT64_C(1) << 22)
#define TARGET_ANON (UINT64_C(1) << 12)
#ifndef TARGET_MAX_FOLIOS
#define TARGET_MAX_FOLIOS 100000u
#endif

static int target_folio_batch(const uint64_t *pages, const uint64_t *flags,
                              size_t count, size_t minimum, size_t *folios,
                              size_t *small_pages) {
    for (size_t i = 0; i < count;) {
        if ((pages[i] & (TARGET_PRESENT | TARGET_SWAPPED)) != TARGET_PRESENT ||
                !(pages[i] & TARGET_PFN_MASK)) return -1;
        if ((flags[i] & (TARGET_HEAD | TARGET_TAIL | TARGET_THP)) == 0) {
            if (!(flags[i] & TARGET_ANON)) return -1;
            ++*small_pages; ++*folios; ++i;
            continue;
        }
        if ((flags[i] & (TARGET_HEAD | TARGET_TAIL | TARGET_THP)) !=
                (TARGET_HEAD | TARGET_THP)) return -1;
        uint64_t first = pages[i] & TARGET_PFN_MASK;
        size_t end = i + 1;
        while (end < count && (flags[end] & TARGET_TAIL)) {
            if ((pages[end] & (TARGET_PRESENT | TARGET_SWAPPED)) != TARGET_PRESENT ||
                    (pages[end] & TARGET_PFN_MASK) != first + end - i ||
                    (flags[end] & (TARGET_HEAD | TARGET_TAIL | TARGET_THP)) !=
                        (TARGET_TAIL | TARGET_THP)) return -1;
            ++end;
        }
        size_t n = end - i;
        if (n < 2 || (n & (n - 1)) || first % n || i % n) return -1;
#ifdef TARGET_MAX_FOLIO_SIZE
        if (n > TARGET_MAX_FOLIO_SIZE / 4096) return -1;
#endif
        if (n < minimum) *small_pages += n;
        ++*folios;
        i = end;
    }
    return 0;
}

static int target_folio_budget(size_t folios) {
    if (folios > TARGET_MAX_FOLIOS) {
        errno = ENOMEM; return -1;
    }
    return 0;
}

static int target_folio_coverage(void *ram, size_t size, size_t minimum) {
    if (!size || size % TARGET_FOLIO_SIZE || (uintptr_t)ram % TARGET_FOLIO_SIZE ||
            minimum < 4096 || minimum % 4096) { errno = EINVAL; return -1; }
    int maps = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    int flags_fd = open("/proc/kpageflags", O_RDONLY | O_CLOEXEC);
    int rc = -1;
    size_t folios = 0, small_pages = 0;
    if (maps < 0 || flags_fd < 0) goto out;
    for (size_t off = 0; off < size; off += TARGET_FOLIO_SIZE) {
        uint64_t pages[512], flags[512];
        off_t index = (off_t)(((uintptr_t)ram + off) / 4096 * sizeof(uint64_t));
        if (pread(maps, pages, sizeof(pages), index) != sizeof(pages)) goto out;
        for (size_t i = 0; i < 512;) {
            if ((pages[i] & (TARGET_PRESENT | TARGET_SWAPPED)) != TARGET_PRESENT ||
                    !(pages[i] & TARGET_PFN_MASK)) goto invalid;
            uint64_t pfn = pages[i] & TARGET_PFN_MASK;
            size_t end = i + 1;
            while (end < 512 && (pages[end] & TARGET_PFN_MASK) == pfn + end - i) ++end;
            size_t bytes = (end - i) * sizeof(uint64_t);
            if (pread(flags_fd, flags + i, bytes, (off_t)(pfn * 8)) != (ssize_t)bytes) goto out;
            i = end;
        }
        if (target_folio_batch(pages, flags, 512, minimum / 4096, &folios, &small_pages)) {
            fprintf(stderr, "TARGET_FOLIO_INVALID offset=%zu minimum=%zu\n", off, minimum);
            goto invalid;
        }
    }
    printf("TARGET_FOLIO_COVERAGE bytes=%zu preferred_minimum=%zu folios=%zu small_bytes=%zu small_limit=none folio_limit=%u\n",
           size, minimum, folios, small_pages * 4096,
           TARGET_MAX_FOLIOS);
    if (target_folio_budget(folios)) goto invalid;
    rc = 0;
    goto out;
invalid:
    errno = ENOMEM;
out:;
    int saved = errno;
    if (maps >= 0) close(maps);
    if (flags_fd >= 0) close(flags_fd);
    errno = saved;
    return rc;
}
#endif
#endif
