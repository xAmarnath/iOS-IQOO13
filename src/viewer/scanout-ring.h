/* Publication memory only: never registered with Gunyah or used for guest DMA.
 * One producer and one consumer, fixed bounds, latest complete frame wins. */
#ifndef SCANOUT_RING_H
#define SCANOUT_RING_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define SCANOUT_WIDTH 1536u
#define SCANOUT_HEIGHT 2048u
#define SCANOUT_BYTES (SCANOUT_WIDTH * SCANOUT_HEIGHT * 4u)
#define SCANOUT_SLOTS 3u
#define SCANOUT_HEADER 4096u
#define SCANOUT_SIZE (SCANOUT_HEADER + SCANOUT_SLOTS * SCANOUT_BYTES)
#define SCANOUT_MAGIC UINT32_C(0x32524653)
enum { SCANOUT_FREE, SCANOUT_WRITING, SCANOUT_READY, SCANOUT_READING };
struct scanout_slot { uint32_t state, reserved; uint64_t sequence, captured_ns; };
struct scanout_ring { uint32_t magic, version, bytes, reserved; struct scanout_slot slot[SCANOUT_SLOTS]; };
_Static_assert(sizeof(struct scanout_ring) <= SCANOUT_HEADER, "Ring header too large");
_Static_assert(__atomic_always_lock_free(4, 0), "Shared slot state must be lock-free");
static inline void *scanout_pixels(struct scanout_ring *r, unsigned i) {
    return (unsigned char *)r + SCANOUT_HEADER + (size_t)i * SCANOUT_BYTES;
}
static inline int scanout_claim(struct scanout_ring *r) {
    /* Prefer unused slots. Reclaim READY only; a READING slot is immutable. */
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned i = 0; i < SCANOUT_SLOTS; ++i) {
            uint32_t expected = pass ? SCANOUT_READY : SCANOUT_FREE;
            if (__atomic_compare_exchange_n(&r->slot[i].state, &expected, SCANOUT_WRITING,
                    0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return (int)i;
        }
    return -1;
}
static inline void scanout_publish(struct scanout_ring *r, unsigned i,
                                   const void *pixels, uint64_t sequence, uint64_t ns) {
    memcpy(scanout_pixels(r, i), pixels, SCANOUT_BYTES);
    r->slot[i].sequence = sequence; r->slot[i].captured_ns = ns;
    __atomic_store_n(&r->slot[i].state, SCANOUT_READY, __ATOMIC_RELEASE);
}
static inline int scanout_acquire(struct scanout_ring *r, uint64_t after) {
    /* Claim before reading metadata: producer may replace a READY slot. */
    int selected = -1;
    uint64_t latest = after;
    for (unsigned i = 0; i < SCANOUT_SLOTS; ++i) {
        uint32_t expected = SCANOUT_READY;
        if (!__atomic_compare_exchange_n(&r->slot[i].state, &expected, SCANOUT_READING,
                0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
        if (r->slot[i].sequence > latest) {
            if (selected >= 0) __atomic_store_n(&r->slot[selected].state, SCANOUT_FREE, __ATOMIC_RELEASE);
            selected = (int)i; latest = r->slot[i].sequence;
        } else __atomic_store_n(&r->slot[i].state, SCANOUT_FREE, __ATOMIC_RELEASE);
    }
    return selected;
}
static inline void scanout_release(struct scanout_ring *r, unsigned i) {
    __atomic_store_n(&r->slot[i].state, SCANOUT_FREE, __ATOMIC_RELEASE);
}
#endif
