#include "vgpu-shared.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    uint32_t offset = 0;
    const uint64_t start = 32u << 20;
    assert(vgpu_response_bytes(VGPU_POOL_SIZE) == start);
    assert(vgpu_response_bytes(8u << 20) == (4u << 20));
    assert(vgpu_response_bytes(64u << 20) == start);
    assert(vgpu_bo_range(VGPU_POOL_SIZE, VGPU_POOL_BASE + start, VGPU_POOL_SIZE - start, &offset));
    assert(offset == start);
    assert(vgpu_bo_range(VGPU_POOL_SIZE, VGPU_POOL_BASE + VGPU_POOL_SIZE - 16384, 16384, &offset));
    uint64_t addresses[] = {0, 0x80000000, VGPU_COMMAND_BASE, VGPU_POOL_BASE,
        VGPU_POOL_BASE + start - 16384, VGPU_POOL_BASE + VGPU_POOL_SIZE,
        VGPU_POOL_BASE + start + 1, UINT64_MAX - 16383};
    for (unsigned i = 0; i < sizeof addresses / sizeof addresses[0]; ++i)
        assert(!vgpu_bo_range(VGPU_POOL_SIZE, addresses[i], 16384, &offset));
    assert(!vgpu_bo_range(VGPU_POOL_SIZE, VGPU_POOL_BASE + VGPU_POOL_SIZE - 16384, 32768, &offset));
    assert(!vgpu_bo_range(VGPU_POOL_SIZE, VGPU_POOL_BASE + start, 0, &offset));
    assert(!vgpu_bo_range(VGPU_POOL_SIZE, VGPU_POOL_BASE + start, UINT64_MAX, &offset));
    assert(!vgpu_bo_range(VGPU_POOL_SIZE + 32768, VGPU_POOL_BASE + start, 16384, &offset));
    uint64_t seed = 1;
    for (unsigned i = 0; i < 20000; ++i) {
        seed = seed * UINT64_C(6364136223846793005) + 1;
        uint64_t a = seed;
        seed = seed * UINT64_C(6364136223846793005) + 1;
        uint64_t b = seed;
        uint64_t as = (a >> 20) | 1, bs = (b >> 20) | 1;
        __uint128_t ae = (__uint128_t)a + as, be = (__uint128_t)b + bs;
        assert(vgpu_ranges_overlap(a, as, b, bs) == (a < be && b < ae));
    }
    puts("SHARED_RANGE_PASS boundaries=14 overlap_fuzz=20000");
}
