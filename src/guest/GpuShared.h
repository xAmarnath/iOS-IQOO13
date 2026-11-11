// SPDX-License-Identifier: BSD-2-Clause-Patent
// Arithmetic shared by the guest allocator and its host boundary tests.
#pragma once
#include <stdint.h>

static inline bool droidvm_gpu_span(uint64_t base, uint64_t bytes, uint64_t address, uint64_t size) {
    return bytes && base <= UINT64_MAX - bytes && size && size <= bytes &&
           address >= base && address - base <= bytes - size;
}

static inline bool droidvm_gpu_shared_window(uint64_t base, uint64_t bytes, uint64_t needed,
                                             uint64_t privateBase, uint64_t privateBytes) {
    // XNU's 16 KiB pages must not alias private RAM or unrelated shared regions.
    if ((base | bytes) & 16383 || !needed || !droidvm_gpu_span(base, bytes, base, needed) ||
        !privateBytes || privateBase > UINT64_MAX - privateBytes) return false;
    return base + bytes <= privateBase || privateBase + privateBytes <= base;
}

// Reserve at most 32 MiB for native responses (16 contexts, each <= 1 MiB,
// plus the renderer's 2 MiB guard). BOs use the remaining nonoverlapping tail.
// Small test pools retain the half split. Caller supplies a lock.
constexpr uint64_t DroidVMGpuPoolBase = 0x201000000ULL;
enum : uint32_t { DroidVMGpuPage = 16384, DroidVMGpuMaxPoolBytes = 2048u << 20,
                  DroidVMGpuResponseBytes = 32u << 20,
                  DroidVMGpuMaxBoPages = (DroidVMGpuMaxPoolBytes - DroidVMGpuResponseBytes) / DroidVMGpuPage,
                  DroidVMGpuNoOffset = UINT32_MAX };
static inline uint64_t droidvm_gpu_response_bytes(uint64_t bytes) {
    return bytes / 2 < DroidVMGpuResponseBytes ? bytes / 2 : uint64_t(DroidVMGpuResponseBytes);
}
static_assert(DroidVMGpuMaxBoPages < UINT32_MAX, "BO length must not alias continuation marker");
static_assert(DroidVMGpuPoolBase >= 0x40000000ULL, "BO pool overlaps GIC");
static_assert(DroidVMGpuPoolBase >= 0x1c0000000ULL, "BO pool overlaps largest private RAM profile");
static_assert(DroidVMGpuPoolBase <= UINT64_MAX - DroidVMGpuMaxPoolBytes, "BO pool address overflow");
static inline bool droidvm_gpu_bo_layout(uint64_t base, uint64_t bytes) {
    return base == DroidVMGpuPoolBase && bytes >= (8u << 20) && bytes <= DroidVMGpuMaxPoolBytes &&
           !(bytes & (2 * DroidVMGpuPage - 1));
}
struct DroidVMGpuBoSpace { uint64_t freeBytes, largestBytes; };
struct DroidVMGpuBoPages {
    // Head stores allocation length, remaining pages use a continuation marker.
    uint32_t pages[DroidVMGpuMaxBoPages];
    DroidVMGpuBoSpace space(uint64_t poolBytes) const {
        DroidVMGpuBoSpace result = {};
        if (!droidvm_gpu_bo_layout(DroidVMGpuPoolBase, poolBytes)) return result;
        uint64_t run = 0;
        for (uint32_t i = 0; i < (poolBytes - droidvm_gpu_response_bytes(poolBytes)) / DroidVMGpuPage; ++i) {
            run = pages[i] ? 0 : run + DroidVMGpuPage;
            if (!pages[i]) result.freeBytes += DroidVMGpuPage;
            if (run > result.largestBytes) result.largestBytes = run;
        }
        return result;
    }
    uint32_t take(uint64_t poolBytes, uint64_t bytes) {
        if (!droidvm_gpu_bo_layout(DroidVMGpuPoolBase, poolBytes) || !bytes ||
            (bytes & (DroidVMGpuPage - 1)) || bytes > poolBytes - droidvm_gpu_response_bytes(poolBytes)) return DroidVMGpuNoOffset;
        const uint32_t total = uint32_t((poolBytes - droidvm_gpu_response_bytes(poolBytes)) / DroidVMGpuPage);
        const uint32_t needed = uint32_t(bytes / DroidVMGpuPage);
        uint32_t run = 0;
        for (uint32_t i = 0; i < total; ++i) {
            run = pages[i] ? 0 : run + 1;
            if (run != needed) continue;
            const uint32_t first = i + 1 - needed;
            pages[first] = needed;
            for (uint32_t j = first + 1; j <= i; ++j) pages[j] = UINT32_MAX;
            return uint32_t(droidvm_gpu_response_bytes(poolBytes)) + first * DroidVMGpuPage;
        }
        return DroidVMGpuNoOffset;
    }
    bool give(uint64_t poolBytes, uint32_t offset, uint64_t bytes) {
        if (!droidvm_gpu_bo_layout(DroidVMGpuPoolBase, poolBytes) || !bytes ||
            ((offset | bytes) & (DroidVMGpuPage - 1)) || offset < droidvm_gpu_response_bytes(poolBytes) ||
            offset >= poolBytes || bytes > poolBytes - offset) return false;
        const uint32_t first = uint32_t((offset - droidvm_gpu_response_bytes(poolBytes)) / DroidVMGpuPage);
        const uint32_t count = uint32_t(bytes / DroidVMGpuPage);
        if (pages[first] != count) return false;
        for (uint32_t j = 1; j < count; ++j) if (pages[first + j] != UINT32_MAX) return false;
        for (uint32_t j = 0; j < count; ++j) pages[first + j] = 0;
        return true;
    }
};
