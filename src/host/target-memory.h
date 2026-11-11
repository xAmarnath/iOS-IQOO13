/* Prepare private target RAM before any Gunyah descriptor is opened.
 * Prefer large host folios to reduce EL2 mapping metadata. Never access or
 * release these pages while a started guest still owns them.
 * Mixed mode restores temporary mTHP policies before opening Gunyah. */
#ifndef TARGET_MEMORY_H
#define TARGET_MEMORY_H
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23
#endif
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif
#define TARGET_FOLIO_SIZE ((size_t)2 * 1024 * 1024)
#include "target-memory-config.h"
#ifndef TARGET_MIN_FOLIO_SIZE
#define TARGET_MIN_FOLIO_SIZE (2 * 1024 * 1024)
#endif
#include "target-folio-check.h"
#ifdef TARGET_MIXED_FOLIOS
#include "target-thp-policy.h"
#endif

struct target_memory {
    void *reservation;
    size_t reserved_size, size;
    unsigned char *ram;
};

/* Require exact, complete accounting for our guarded VMA. Checking only
 * MADV_COLLAPSE's return value would miss subsequent folio splits. */
static int target_memory_coverage(FILE *smaps, uintptr_t begin, size_t size) {
    uintptr_t end = begin + size, covered = begin;
    size_t huge = 0, resident = 0, locked = 0, swapped = 0;
    unsigned counts[4] = {0};
    unsigned segments = 0;
    int selected = 0, bad = !size || end < begin;
    char line[512];
    while (fgets(line, sizeof(line), smaps)) {
        uintptr_t low, high;
        if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR, &low, &high) == 2) {
            selected = low < end && high > begin;
            if (selected) {
                if (low != covered || low < begin || high > end || high <= low) bad = 1;
                covered = high;
                ++segments;
            }
            continue;
        }
        if (!selected) continue;
        size_t kb;
        if (sscanf(line, "AnonHugePages: %zu kB", &kb) == 1) { huge += kb * 1024; ++counts[0]; }
        if (sscanf(line, "Rss: %zu kB", &kb) == 1) { resident += kb * 1024; ++counts[1]; }
        if (sscanf(line, "Locked: %zu kB", &kb) == 1) { locked += kb * 1024; ++counts[2]; }
        if (sscanf(line, "Swap: %zu kB", &kb) == 1) { swapped += kb * 1024; ++counts[3]; }
    }
    if (ferror(smaps)) bad = 1;
    for (unsigned i = 0; i < 4; ++i) if (counts[i] != segments) bad = 1;
    printf("TARGET_RAM_COVERAGE bytes=%zu huge=%zu resident=%zu locked=%zu swap=%zu\n",
           size, huge, resident, locked, swapped);
    if (bad || covered != end ||
            (TARGET_MIN_FOLIO_SIZE == 2 * 1024 * 1024 && huge != size) ||
            resident != size || locked != size || swapped) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

static int target_memory_verify(const struct target_memory *memory) {
    FILE *maps = fopen("/proc/self/smaps", "r");
    if (!maps) return -1;
    int rc = target_memory_coverage(maps, (uintptr_t)memory->ram, memory->size);
    int saved = errno;
    fclose(maps);
    errno = saved;
#if TARGET_MIN_FOLIO_SIZE < 2 * 1024 * 1024
    if (!rc) rc = target_folio_coverage(memory->ram, memory->size, TARGET_MIN_FOLIO_SIZE);
#endif
    return rc;
}

static int target_memory_release(struct target_memory *memory) {
    if (!memory->reservation) return 0;
    if (munmap(memory->reservation, memory->reserved_size)) return -1;
    memset(memory, 0, sizeof(*memory));
    return 0;
}

static int target_memory_allocate(struct target_memory *memory, size_t size,
                                 uint64_t gpa, const volatile sig_atomic_t *cancelled) {
    const char *stage = "reserve";
    size_t completed = 0;
    long page = sysconf(_SC_PAGESIZE);
    if (memory->reservation || page != 4096 || !size || size % TARGET_FOLIO_SIZE ||
            gpa % TARGET_FOLIO_SIZE || size > SIZE_MAX - TARGET_FOLIO_SIZE - 8192) {
        errno = EINVAL; return -1;
    }
    if (*cancelled) { errno = EINTR; return -1; }
    memory->reserved_size = size + TARGET_FOLIO_SIZE + 8192;
    memory->reservation = mmap(NULL, memory->reserved_size, PROT_NONE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory->reservation == MAP_FAILED) {
        int saved = errno;
        fprintf(stderr, "TARGET_RAM_FAILURE stage=%s completed=%zu requested=%zu errno=%d\n",
                stage, completed, size, saved);
        errno = saved;
        memset(memory, 0, sizeof(*memory)); return -1;
    }
    uintptr_t aligned = ((uintptr_t)memory->reservation + 4096 + TARGET_FOLIO_SIZE - 1) &
                        ~(uintptr_t)(TARGET_FOLIO_SIZE - 1);
    memory->ram = (unsigned char *)aligned;
    memory->size = size;
    /* Guard VMAs keep smaps accounting isolated from neighboring allocations. */
    stage = "protect";
    if (mprotect(memory->ram, size, PROT_READ | PROT_WRITE)) goto fail;
    stage = "dontdump";
    if (madvise(memory->ram, size, MADV_DONTDUMP)) goto fail;
    /* Backend helpers must not inherit private guest RAM or force its locked
     * folios through fork/COW. This also permits reserving RAM before GPU IPC. */
    stage = "dontfork";
    if (madvise(memory->ram, size, MADV_DONTFORK)) goto fail;
    stage = "hugepage-advice";
    if (madvise(memory->ram, size, MADV_HUGEPAGE)) goto fail;
    /* Lock pages as they fault in. Delaying the lock until after the complete
     * allocation lets Android swap and split newly allocated large folios. */
    stage = "lock-onfault";
    if (mlock2(memory->ram, size, MLOCK_ONFAULT)) goto fail;
    for (size_t off = 0; off < size; off += TARGET_FOLIO_SIZE) {
        stage = "cancelled";
        if (*cancelled) { errno = EINTR; goto fail; }
        void *chunk = memory->ram + off;
        /* Fault within the pre-locked VMA; every populated chunk stays resident. */
        stage = "populate";
        if (madvise(chunk, TARGET_FOLIO_SIZE, MADV_POPULATE_WRITE)) goto fail;
        completed += TARGET_FOLIO_SIZE;
    }
#ifndef TARGET_MIXED_FOLIOS
    /* Mixed mode faults with all supported large-page sizes and synchronous
     * madvise defrag enabled. Do not recopy those folios through COLLAPSE:
     * the final PFN/compound-page gate verifies the actual allocation. */
    completed = 0;
    for (size_t off = 0; off < size; off += TARGET_FOLIO_SIZE) {
        stage = "cancelled";
        if (*cancelled) { errno = EINTR; goto fail; }
        void *chunk = memory->ram + off;
        stage = "collapse";
        if (madvise(chunk, TARGET_FOLIO_SIZE, MADV_COLLAPSE)) {
#if TARGET_MIN_FOLIO_SIZE == 2 * 1024 * 1024
            goto fail;
#else
            /* Accept smaller pages only after full compound-page accounting
             * meets the explicitly configured mapping and small-page budgets. */
            if (errno != ENOMEM && errno != EAGAIN) goto fail;
#endif
        }
        completed += TARGET_FOLIO_SIZE;
    }
#endif
    if (*cancelled) { errno = EINTR; goto fail; }
#if defined(TARGET_ALLOCATION_DIAGNOSTICS) && defined(TARGET_MIXED_FOLIOS)
    puts("TARGET_ALLOCATION_DIAGNOSTIC stage=after_populate");
    (void)target_memory_verify(memory);
    (void)target_folio_coverage(memory->ram, size, TARGET_MIN_FOLIO_SIZE);
#endif
    stage = "lock";
    if (mlock(memory->ram, size)) goto fail;
#ifdef TARGET_MAX_FOLIO_SIZE
    /* Prevent later khugepaged collapse after restoring global allocation
     * policies. Verify the surviving folios before any VM owns these pages. */
    stage = "freeze-folio-size";
    if (madvise(memory->ram, size, MADV_NOHUGEPAGE)) goto fail;
#endif
    if (*cancelled) { errno = EINTR; goto fail; }
    stage = "verify";
    if (target_memory_verify(memory)) goto fail;
    printf("TARGET_RAM_READY preferred_folio_bytes=%u locked=1 vm_created=0\n",
           (unsigned)TARGET_MIN_FOLIO_SIZE);
    return 0;
fail:;
    int saved = errno;
    fprintf(stderr, "TARGET_RAM_FAILURE stage=%s completed=%zu requested=%zu errno=%d\n",
            stage, completed, size, saved);
    /* Only called before creating a VM; no guest owns these pages. */
    if (target_memory_release(memory)) perror("release failed target RAM preparation");
    errno = saved;
    return -1;
}

static int target_memory_prepare(struct target_memory *memory, size_t size,
                                 uint64_t gpa, const volatile sig_atomic_t *cancelled) {
#ifdef TARGET_MIXED_FOLIOS
    struct target_thp_policy policy = {0};
    if (*cancelled) { errno = EINTR; return -1; }
    if (target_policy_begin(&policy)) return -1;
    int rc = target_memory_allocate(memory, size, gpa, cancelled);
    int saved = errno;
    if (target_policy_restore(&policy)) {
        /* No VM exists. Refuse launch even when allocation itself passed. */
        if (target_memory_release(memory)) perror("release RAM after policy restoration failure");
        return -1;
    }
    errno = saved;
    return rc;
#else
    return target_memory_allocate(memory, size, gpa, cancelled);
#endif
}
#endif
