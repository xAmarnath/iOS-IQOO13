#include "vgpu-queue.h"
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(struct vgpu_header) == 24, "GPU header ABI");
_Static_assert(sizeof(struct vgpu_desc) == 16, "virtqueue ABI");
static int fault(struct vgpu_device *d) { d->status |= 64; d->errors++; return -1; }
static int span(uint64_t base, uint64_t size, uint64_t address, uint64_t bytes) {
    return bytes && bytes <= size && address >= base && address - base <= size - bytes;
}
static int overlap(uint64_t a, uint64_t n, uint64_t b, uint64_t m) {
    return a <= b ? b - a < n : a - b < m;
}
void *vgpu_dma(struct vgpu_device *d, uint64_t address, size_t bytes) {
    return span(d->base, d->size, address, bytes) ? d->shared + (address - d->base) : NULL;
}
int vgpu_init(struct vgpu_device *d, void *memory, uint64_t base, size_t bytes,
              vgpu_dispatch dispatch, vgpu_reset reset, void *opaque) {
    memset(d, 0, sizeof(*d));
    if (!memory || !dispatch || !bytes || base > UINT64_MAX - bytes ||
        (base & 16383) || (bytes & 16383)) return -1;
    d->request = malloc(VGPU_REQUEST_MAX);
    d->reply = malloc(VGPU_REPLY_MAX);
    if (!d->request || !d->reply) { free(d->request); free(d->reply); memset(d, 0, sizeof(*d)); return -1; }
    d->shared = memory; d->base = base; d->size = bytes;
    d->dispatch = dispatch; d->reset = reset; d->opaque = opaque;
    return 0;
}
void vgpu_destroy(struct vgpu_device *d) {
    if (d->reset) d->reset(d->opaque);
    free(d->request); free(d->reply); memset(d, 0, sizeof(*d));
}
static int ring_bounds(struct vgpu_device *d, const struct vgpu_queue *q) {
    if (!q->num || q->num > VGPU_QUEUE_MAX || (q->num & (q->num - 1)) ||
        (q->desc & 15) || (q->avail & 1) || (q->used & 3)) return 0;
    const uint64_t a[] = {q->desc, q->avail, q->used};
    const size_t n[] = {16 * q->num, 6 + 2 * q->num, 6 + 8 * q->num};
    for (unsigned i = 0; i < 3; i++) {
        if (!vgpu_dma(d, a[i], n[i])) return 0;
        for (unsigned j = 0; j < i; j++) if (overlap(a[i], n[i], a[j], n[j])) return 0;
    }
    /* No control/cursor ring may overwrite the other queue. */
    for (unsigned other = 0; other < 2; other++) {
        const struct vgpu_queue *b = &d->queue[other];
        if (b == q || !b->ready) continue;
        const uint64_t ba[] = {b->desc, b->avail, b->used};
        const size_t bn[] = {16 * b->num, 6 + 2 * b->num, 6 + 8 * b->num};
        for (unsigned i = 0; i < 3; i++) for (unsigned j = 0; j < 3; j++)
            if (overlap(a[i], n[i], ba[j], bn[j])) return 0;
    }
    return 1;
}
static int buffer_bounds(struct vgpu_device *d, const struct vgpu_desc *entry) {
    if (!vgpu_dma(d, entry->address, entry->length)) return 0;
    for (unsigned i = 0; i < 2; i++) {
        const struct vgpu_queue *q = &d->queue[i];
        if (q->ready && (overlap(entry->address, entry->length, q->desc, 16 * q->num) ||
            overlap(entry->address, entry->length, q->avail, 6 + 2 * q->num) ||
            overlap(entry->address, entry->length, q->used, 6 + 8 * q->num))) return 0;
    }
    return 1;
}
int vgpu_notify(struct vgpu_device *d, unsigned which) {
    if (which >= 2 || (d->status & 0xcf) != 15) return fault(d);
    struct vgpu_queue *q = &d->queue[which];
    if (!q->ready || !ring_bounds(d, q)) return fault(d);
    unsigned char *descs = vgpu_dma(d, q->desc, 16 * q->num);
    unsigned char *avail = vgpu_dma(d, q->avail, 6 + 2 * q->num);
    unsigned char *used = vgpu_dma(d, q->used, 6 + 8 * q->num);
    uint16_t end = __atomic_load_n((uint16_t *)(void *)(avail + 2), __ATOMIC_ACQUIRE);
    if ((uint16_t)(end - q->consumed) > q->num) return fault(d);
    while (q->consumed != end) {
        uint16_t head, index;
        memcpy(&head, avail + 4 + 2 * (q->consumed % q->num), 2);
        struct vgpu_desc entries[VGPU_QUEUE_MAX];
        unsigned char seen[VGPU_QUEUE_MAX] = {0};
        unsigned count = 0, writes = 0;
        size_t read_bytes = 0, write_bytes = 0;
        index = head;
        for (;;) {
            if (index >= q->num || seen[index] || count == q->num) return fault(d);
            seen[index] = 1;
            struct vgpu_desc *e = &entries[count];
            memcpy(e, descs + 16 * index, sizeof(*e));
            if ((e->flags & ~3u) || !buffer_bounds(d, e)) return fault(d);
            if (e->flags & 2) {
                writes++;
                if (e->length > VGPU_REPLY_MAX - write_bytes) return fault(d);
                write_bytes += e->length;
            } else {
                if (writes || e->length > VGPU_REQUEST_MAX - read_bytes) return fault(d);
                read_bytes += e->length;
            }
            for (unsigned i = 0; i < count; i++)
                if (((entries[i].flags | e->flags) & 2) &&
                    overlap(e->address, e->length, entries[i].address, entries[i].length)) return fault(d);
            count++;
            if (!(e->flags & 1)) break;
            index = e->next;
        }
        if (read_bytes < sizeof(struct vgpu_header) || write_bytes < sizeof(struct vgpu_header)) return fault(d);
        size_t at = 0;
        for (unsigned i = 0; i < count - writes; i++) {
            memcpy(d->request + at, vgpu_dma(d, entries[i].address, entries[i].length), entries[i].length);
            at += entries[i].length;
        }
        /* Backend sees an immutable private snapshot. Refuse fences until the
         * real fence retirement path can delay completion, never fake one. */
        struct vgpu_header header;
        memcpy(&header, d->request, sizeof(header));
        size_t reply;
        memset(d->reply, 0, write_bytes);
        if (header.flags || header.ring || header.padding[0] || header.padding[1] || header.padding[2]) {
            struct vgpu_header error = {.type = VGPU_RESP_INVALID};
            memcpy(d->reply, &error, sizeof(error)); reply = sizeof(error);
        } else {
            reply = d->dispatch(d->opaque, which, d->request, read_bytes, d->reply, write_bytes);
        }
        if (reply < sizeof(struct vgpu_header) || reply > write_bytes) return fault(d);
        at = 0;
        for (unsigned i = count - writes; i < count && at < reply; i++) {
            size_t n = entries[i].length < reply - at ? entries[i].length : reply - at;
            memcpy(vgpu_dma(d, entries[i].address, entries[i].length), d->reply + at, n); at += n;
        }
        uint32_t completion[2] = {head, (uint32_t)reply};
        memcpy(used + 4 + 8 * (q->produced % q->num), completion, sizeof(completion));
        q->consumed++; q->produced++; d->completions++;
        __atomic_store_n((uint16_t *)(void *)(used + 2), q->produced, __ATOMIC_RELEASE);
        uint16_t flags; memcpy(&flags, avail, 2);
        if (!(flags & 1)) d->irq |= 1;
    }
    return 0;
}
int vgpu_mmio(struct vgpu_device *d, uint32_t offset, unsigned bytes, int write, uint32_t *value) {
    if (!value || bytes != 4 || (offset & 3) || offset >= 0x1000) return fault(d);
    struct vgpu_queue *q = d->queue_sel < 2 ? &d->queue[d->queue_sel] : NULL;
    if (!write) {
        *value = 0;
        switch (offset) {
        case 0x00: *value = 0x74726976; break;
        case 0x04: *value = 2; break;
        case 0x08: *value = 16; break;
        case 0x0c: *value = 0x554d4551; break;
        case 0x10: *value = d->feature_sel < 2 ? (uint32_t)(VGPU_FEATURES >> (32 * d->feature_sel)) : 0; break;
        case 0x34: *value = q ? (d->queue_sel ? 4 : VGPU_QUEUE_MAX) : 0; break;
        case 0x44: *value = q ? q->ready : 0; break;
        case 0x60: *value = d->irq; break;
        case 0x70: *value = d->status; break;
        case 0x10c: *value = 1; break; /* one DRM capset, zero scanouts */
        default: break;
        }
        return 0;
    }
    switch (offset) {
    case 0x14: d->feature_sel = *value; return 0;
    case 0x24: d->driver_sel = *value; return 0;
    case 0x20:
        if (d->status & 8 || d->driver_sel > 1) return fault(d);
        d->negotiated = (d->negotiated & ~(UINT64_C(0xffffffff) << (32 * d->driver_sel))) |
                        ((uint64_t)*value << (32 * d->driver_sel)); return 0;
    case 0x30: d->queue_sel = *value; return 0;
    case 0x50: return vgpu_notify(d, *value);
    case 0x64: d->irq &= ~*value; return 0;
    case 0x70:
        if (!*value) {
            if (d->reset) d->reset(d->opaque);
            memset(d->queue, 0, sizeof(d->queue));
            d->negotiated = 0; d->status = d->irq = 0;
            d->feature_sel = d->driver_sel = d->queue_sel = 0; return 0;
        }
        if ((*value & ~0x8fu) || (d->status & ~*value) ||
            ((*value & 8) && ((*value & 3) != 3 || !(d->negotiated & (UINT64_C(1) << 32)) ||
             (d->negotiated & ~VGPU_FEATURES))) || ((*value & 4) && (*value & 11) != 11)) return fault(d);
        d->status = *value; return 0;
    case 0x44:
        if (!q || *value > 1 || (q->ready && *value)) return fault(d);
        if (*value && !ring_bounds(d, q)) return fault(d);
        q->ready = *value; q->consumed = q->produced = 0;
        if (*value) memset(vgpu_dma(d, q->used, 4), 0, 4);
        return 0;
    case 0x38:
        if (!q || q->ready || !*value || *value > (d->queue_sel ? 4 : VGPU_QUEUE_MAX) || (*value & (*value - 1))) return fault(d);
        q->num = *value; return 0;
    case 0x80: case 0x84: case 0x90: case 0x94: case 0xa0: case 0xa4: {
        if (!q || q->ready) return fault(d);
        uint64_t *address = offset < 0x90 ? &q->desc : offset < 0xa0 ? &q->avail : &q->used;
        unsigned shift = offset & 4 ? 32 : 0;
        *address = (*address & ~(UINT64_C(0xffffffff) << shift)) | ((uint64_t)*value << shift); return 0;
    }
    default: return fault(d);
    }
}
