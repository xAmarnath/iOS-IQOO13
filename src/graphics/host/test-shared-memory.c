#define _GNU_SOURCE
#include <assert.h>
#include "vgpu-shared-memory.h"
int main(void) {
    uint64_t p[512], f[512];
    for (size_t i = 0; i < 512; ++i) {
        p[i] = VGPU_PAGE_PRESENT | (1024 + i);
        f[i] = VGPU_PAGE_THP | (i ? VGPU_PAGE_TAIL : VGPU_PAGE_HEAD | VGPU_PAGE_SWAPBACKED);
    }
    assert(!vgpu_shared_folio_valid(p, f));
    f[1] |= VGPU_PAGE_SWAPBACKED; assert(!vgpu_shared_folio_valid(p, f)); f[1] &= ~VGPU_PAGE_SWAPBACKED;
    p[100] |= VGPU_PAGE_SWAPPED; assert(vgpu_shared_folio_valid(p, f)); p[100] &= ~VGPU_PAGE_SWAPPED;
    ++p[200]; assert(vgpu_shared_folio_valid(p, f)); --p[200];
    f[511] &= ~VGPU_PAGE_TAIL; assert(vgpu_shared_folio_valid(p, f)); f[511] |= VGPU_PAGE_TAIL;
    f[0] &= ~VGPU_PAGE_SWAPBACKED; assert(vgpu_shared_folio_valid(p, f)); f[0] |= VGPU_PAGE_SWAPBACKED;
    for (size_t i = 0; i < 512; ++i) ++p[i];
    assert(vgpu_shared_folio_valid(p, f));
    size_t base = 0, large = 0;
    /* Real fallback: no physical contiguity or PFN alignment is required. */
    for (size_t i = 0; i < 512; ++i) {
        p[i] = VGPU_PAGE_PRESENT | (1001 + i * 7);
        f[i] = VGPU_PAGE_SWAPBACKED;
    }
    assert(!vgpu_shared_pages_valid(p, f, &base, &large));
    assert(base == 512 && large == 0);
    p[31] |= VGPU_PAGE_SWAPPED;
    assert(vgpu_shared_pages_valid(p, f, &base, &large)); p[31] &= ~VGPU_PAGE_SWAPPED;
    p[31] &= ~VGPU_PAGE_PRESENT;
    assert(vgpu_shared_pages_valid(p, f, &base, &large)); p[31] |= VGPU_PAGE_PRESENT;
    f[1] = 0; assert(vgpu_shared_pages_valid(p, f, &base, &large)); f[1] = VGPU_PAGE_SWAPBACKED;
    f[1] |= VGPU_PAGE_TAIL | VGPU_PAGE_THP;
    assert(vgpu_shared_pages_valid(p, f, &base, &large)); f[1] = VGPU_PAGE_SWAPBACKED;
    /* Mixed compound/base chunks, with correct boundaries and real PFNs. */
    for (size_t i = 0; i < 16; ++i) {
        p[i] = VGPU_PAGE_PRESENT | (2048 + i);
        f[i] = VGPU_PAGE_THP | (i ? VGPU_PAGE_TAIL : VGPU_PAGE_HEAD | VGPU_PAGE_SWAPBACKED);
    }
    base = large = 0;
    assert(!vgpu_shared_pages_valid(p, f, &base, &large));
    assert(base == 496 && large == 1);
    ++p[8]; assert(vgpu_shared_pages_valid(p, f, &base, &large)); --p[8];
    f[15] = VGPU_PAGE_SWAPBACKED;
    assert(vgpu_shared_pages_valid(p, f, &base, &large));
    for (size_t i = 0; i < 512; ++i) {
        p[i] = VGPU_PAGE_PRESENT | (4096 + i);
        f[i] = VGPU_PAGE_THP | (i ? VGPU_PAGE_TAIL : VGPU_PAGE_HEAD | VGPU_PAGE_SWAPBACKED);
    }
    base = large = 0;
    assert(!vgpu_shared_pages_valid(p, f, &base, &large));
    assert(base == 0 && large == 1);
    assert(vgpu_shared_prepare(-1, 0, 1) == MAP_FAILED && errno == EINVAL);
    assert(vgpu_shared_prepare(-1, 4096, 1) == MAP_FAILED && errno == EINVAL);
    assert(vgpu_shared_prepare(-1, VGPU_SHARED_FOLIO, 1) == MAP_FAILED);
    assert(vgpu_shared_verify(MAP_FAILED, VGPU_SHARED_FOLIO) < 0);
    puts("SHARED_MEMORY_VALIDATION_PASS scattered_base_pages=1 mixed_folios=1 malformed_folios_and_allocation_errors=1");
}
