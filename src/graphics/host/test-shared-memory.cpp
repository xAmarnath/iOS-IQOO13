#include "../../guest/GpuShared.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void allocatorTests(uint64_t pool) {
    constexpr uint64_t page = DroidVMGpuPage;
    const uint64_t start = pool <= (64u << 20) ? pool / 2 : (32u << 20);
    const uint64_t capacity = pool - start;
    const uint32_t total = uint32_t(capacity / page);
    DroidVMGpuBoPages arena = {};
    assert(arena.space(pool).freeBytes == capacity);
    assert(arena.space(pool).largestBytes == capacity);
    assert(!droidvm_gpu_bo_layout(0x22000000, pool));
    assert(!droidvm_gpu_bo_layout(0x80000000, pool));
    assert(!droidvm_gpu_bo_layout(0x23000000, pool)); // stale pool base
    assert(!droidvm_gpu_bo_layout(0x200000000ULL, pool)); // command window
    assert(droidvm_gpu_bo_layout(DroidVMGpuPoolBase, pool));
    assert(!droidvm_gpu_bo_layout(DroidVMGpuPoolBase, pool + page));
    assert(arena.take(pool, 0) == DroidVMGpuNoOffset);
    assert(arena.take(pool, page - 1) == DroidVMGpuNoOffset);
    assert(arena.take(pool, capacity + page) == DroidVMGpuNoOffset);
    uint32_t a = arena.take(pool, 4 * page), b = arena.take(pool, 3 * page);
    assert(a == start && b == a + 4 * page);
    assert(arena.space(pool).freeBytes == capacity - 7 * page);
    assert(!arena.give(pool, a, 3 * page)); // truncated release
    assert(!arena.give(pool, a + page, 3 * page)); // continuation
    assert(!arena.give(pool, 0, page));
    assert(arena.give(pool, a, 4 * page));
    assert(arena.space(pool).freeBytes == capacity - 3 * page);
    assert(arena.space(pool).largestBytes == capacity - 7 * page);
    assert(!arena.give(pool, a, 4 * page));
    assert(arena.take(pool, 5 * page) == b + 3 * page); // cannot bridge a live BO
    assert(arena.take(pool, 4 * page) == a);
    arena = {};
    a = arena.take(pool, capacity);
    assert(a == start && arena.take(pool, page) == DroidVMGpuNoOffset);
    assert(arena.space(pool).freeBytes == 0 && arena.space(pool).largestBytes == 0);
    assert(arena.give(pool, a, capacity));
    if (total > 65535) {
        a = arena.take(pool, 65536ULL * page);
        assert(a == start && arena.pages[0] == 65536);
        assert(!arena.give(pool, a, page));
        assert(arena.give(pool, a, 65536ULL * page));
    }
    // Exercise the last page and a fragmented arena with ample total space.
    a = arena.take(pool, capacity - page);
    b = arena.take(pool, page);
    assert(b == pool - page);
    assert(arena.give(pool, a, capacity - page));
    assert(arena.take(pool, capacity) == DroidVMGpuNoOffset);
    assert(arena.give(pool, b, page));

    // Independent occupancy model checks fragmentation, retention (uncertain
    // imports deliberately stay allocated), repeated reuse and exhaustion.
    struct Live { uint32_t offset; uint64_t bytes; } live[512] = {};
    bool occupied[DroidVMGpuMaxBoPages] = {};
    uint32_t random = 0x690bd123, count = 0;
    for (unsigned iteration = 0; iteration < 20000; ++iteration) {
        random = random * 1664525 + 1013904223;
        if (count && ((random & 3) == 0 || count == 512)) {
            uint32_t which = (random >> 4) % count;
            Live old = live[which];
            assert(arena.give(pool, old.offset, old.bytes));
            for (uint32_t i = 0; i < old.bytes / page; ++i) occupied[(old.offset - start) / page + i] = false;
            live[which] = live[--count];
        } else {
            uint32_t pages = 1 + (random >> 4) % 128, run = 0, expected = DroidVMGpuNoOffset;
            for (uint32_t i = 0; i < total; ++i) {
                run = occupied[i] ? 0 : run + 1;
                if (run == pages) { expected = uint32_t(start + (i + 1 - pages) * page); break; }
            }
            uint32_t got = arena.take(pool, pages * page);
            assert(got == expected);
            if (got == DroidVMGpuNoOffset) continue;
            live[count++] = {got, pages * page};
            for (uint32_t i = 0; i < pages; ++i) occupied[(got - start) / page + i] = true;
        }
    }
    while (count) { --count; assert(arena.give(pool, live[count].offset, live[count].bytes)); }
    assert(arena.take(pool, capacity) == start);
    printf("GPU_SHARED_ALLOCATOR_OK pool_mib=%llu random_cases=20000\n", (unsigned long long)(pool >> 20));
}

int main() {
    allocatorTests(8u << 20);
    allocatorTests(64u << 20);
    allocatorTests((64u << 20) + 2 * DroidVMGpuPage);
    allocatorTests(1024u << 20);
    allocatorTests(1536u << 20);
    allocatorTests(DroidVMGpuMaxPoolBytes);
    DroidVMGpuBoPages invalid = {};
    const uint64_t oversized = uint64_t(DroidVMGpuMaxPoolBytes) + 2 * DroidVMGpuPage;
    assert(!droidvm_gpu_bo_layout(DroidVMGpuPoolBase, oversized));
    assert(invalid.take(oversized, DroidVMGpuPage) == DroidVMGpuNoOffset);
    assert(invalid.space(oversized).freeBytes == 0);
    const uint64_t base = 0x200000000ULL, size = 16 << 20;
    const uint64_t profiles[] = {2ULL << 30, 3ULL << 30, 5ULL << 30};
    for (uint64_t ram : profiles) {
        assert(droidvm_gpu_shared_window(base, 0x81000000ULL, 0x81000000ULL, 0x80000000, ram));
        assert(!droidvm_gpu_shared_window(0x140000000ULL, 0x81000000ULL, 1, 0x80000000, 5ULL << 30));
    }
    assert(droidvm_gpu_shared_window(base, size, 9 << 20, 0x80000000, 3ULL << 30));
    assert(!droidvm_gpu_shared_window(base + 4096, size, 9 << 20, 0x80000000, 3ULL << 30));
    assert(!droidvm_gpu_shared_window(base, size, size + 1, 0x80000000, 3ULL << 30));
    assert(!droidvm_gpu_shared_window(base, size, 1, base + size - 1, 0x400000));
    assert(!droidvm_gpu_shared_window(base, size, 1, UINT64_MAX - 1, 0x400000));
    assert(!droidvm_gpu_shared_window(UINT64_MAX - 16383, 16384, 1, 0x80000000, 3ULL << 30));
    for (uint64_t n = 1; n <= size; n <<= 1) {
        assert(droidvm_gpu_span(base, size, base, n));
        assert(droidvm_gpu_span(base, size, base + size - n, n));
        assert(!droidvm_gpu_span(base, size, base - 1, n));
        assert(!droidvm_gpu_span(base, size, base + size - n + 1, n));
        assert(!droidvm_gpu_span(base, size, 0x80000000, n));
    }
    assert(!droidvm_gpu_span(base, size, base, size + 1));
    assert(!droidvm_gpu_span(base, size, base, 0));
    assert(!droidvm_gpu_span(UINT64_MAX - 3, 8, UINT64_MAX - 2, 1));
    puts("GPU_SHARED_BOUNDARIES_OK");
}
