/* Renderer SHARE layout. Responses occupy a bounded prefix; GPU BOs may use
 * only aligned ranges after that prefix. Never import private RAM. */
#ifndef OPENOOS_VGPU_SHARED_H
#define OPENOOS_VGPU_SHARED_H
#include "vgpu-ipc.h"
#define VGPU_PAGE_SIZE 16384u
#define VGPU_RESPONSE_BYTES (32u << 20)
static inline uint64_t vgpu_response_bytes(uint64_t pool_size) {
    return pool_size / 2 < VGPU_RESPONSE_BYTES ? pool_size / 2 : VGPU_RESPONSE_BYTES;
}
static inline int vgpu_bo_range(uint64_t pool_size, uint64_t address,
                                uint64_t length, uint32_t *offset) {
    if (pool_size < (8u << 20) || pool_size > VGPU_POOL_SIZE ||
        (pool_size & (VGPU_PAGE_SIZE * 2 - 1)) || !length ||
        address < VGPU_POOL_BASE || ((address | length) & (VGPU_PAGE_SIZE - 1))) return 0;
    uint64_t at = address - VGPU_POOL_BASE;
    if (at < vgpu_response_bytes(pool_size) || at > pool_size || length > pool_size - at) return 0;
    *offset = (uint32_t)at;
    return 1;
}
static inline int vgpu_ranges_overlap(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
    /* Difference form also handles inputs near UINT64_MAX without overflow. */
    return a <= b ? b - a < as : a - b < bs;
}
#endif
