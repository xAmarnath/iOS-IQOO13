#define _GNU_SOURCE
#define TARGET_MIXED_FOLIOS
#include "target-memory-config.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#define TARGET_FOLIO_SIZE ((size_t)2 * 1024 * 1024)
#include "target-folio-check.h"

int main(void) {
    uint64_t pages[512], flags[512];
    for (unsigned i = 0; i < 512; ++i) {
        pages[i] = TARGET_PRESENT | (512 + i);
        flags[i] = TARGET_THP | (i % 16 ? TARGET_TAIL : TARGET_HEAD);
    }
    size_t folios = 0, small = 0;
    assert(!target_folio_batch(pages, flags, 512, 16, &folios, &small) && folios == 32 && small == 0);
    flags[16] = 0;
    assert(target_folio_batch(pages, flags, 512, 16, &folios, &small) == -1);
    flags[16] = TARGET_THP | TARGET_HEAD;
    pages[20] |= TARGET_SWAPPED;
    assert(target_folio_batch(pages, flags, 512, 16, &folios, &small) == -1);
    pages[20] &= ~TARGET_SWAPPED;
    pages[20] += 1;
    assert(target_folio_batch(pages, flags, 512, 16, &folios, &small) == -1);
    pages[20] -= 1;
    flags[8] = TARGET_HEAD | TARGET_THP;
    assert(!target_folio_batch(pages, flags, 512, 16, &folios, &small) && small == 16);
    flags[8] = TARGET_TAIL | TARGET_THP;
    pages[0] &= TARGET_PRESENT;
    assert(target_folio_batch(pages, flags, 512, 16, &folios, &small) == -1);
    /* Unmapped memory must fail the real pagemap reader, never look resident. */
    assert(target_folio_coverage((void *)(uintptr_t)0x200000, TARGET_FOLIO_SIZE, 65536) == -1);
    assert(!target_folio_budget(TARGET_MAX_FOLIOS));
    assert(target_folio_budget(TARGET_MAX_FOLIOS + 1) == -1 && errno == ENOMEM);
    /* Full 2 GiB of 16 KiB folios: valid small backing at the metadata bound.
     * Entirely scattered 4 KiB backing still exceeds that count budget. */
    for (unsigned i = 0; i < 512; ++i) {
        pages[i] = TARGET_PRESENT | (512 + i);
        flags[i] = TARGET_THP | (i % 4 ? TARGET_TAIL : TARGET_HEAD);
    }
    folios = small = 0;
    for (unsigned i = 0; i < 1024; ++i)
        assert(!target_folio_batch(pages, flags, 512, 16, &folios, &small));
    assert(folios == 131072 && small == 524288 && !target_folio_budget(folios));
    assert(target_folio_budget(524288) == -1);
    /* Real aligned PFN batches: accept 1 MiB, reject an entire 2 MiB folio. */
    for (unsigned i = 0; i < 512; ++i) {
        pages[i] = TARGET_PRESENT | (512 + i);
        flags[i] = TARGET_THP | (i % 256 ? TARGET_TAIL : TARGET_HEAD);
    }
    folios = small = 0;
    assert(!target_folio_batch(pages, flags, 512, 16, &folios, &small) && folios == 2 && small == 0);
    flags[256] = TARGET_THP | TARGET_TAIL;
    assert(target_folio_batch(pages, flags, 512, 16, &folios, &small) == -1);
    for (unsigned i = 0; i < 512; ++i) {
        pages[i] = TARGET_PRESENT | (512 + i); flags[i] = TARGET_ANON;
    }
    folios = small = 0;
    assert(!target_folio_batch(pages, flags, 512, 16, &folios, &small) && folios == 512 && small == 512);
    puts("TARGET_FOLIO_TEST_PASS mixed_folios=1 invalid_pages_rejected=1 budget_boundary_checked=1");
}
