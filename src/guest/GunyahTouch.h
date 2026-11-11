/* SPDX-License-Identifier: MIT
 * Ordered full contact snapshots. Little endian; no private guest DMA. */
#ifndef DROIDVM_GUNYAH_TOUCH_H
#define DROIDVM_GUNYAH_TOUCH_H
#include <stdint.h>
#define GT_MAGIC UINT32_C(0x31544347)
#define GT_VERSION 1u
#define GT_CONTACTS 10u
#define GT_POLL UINT64_C(0x10000040)
#define GT_ACK UINT64_C(0x10000044)
#define GT_DATA UINT64_C(0x10000080)
struct gt_contact { uint32_t id, x, y; };
struct gt_frame {
    uint32_t magic, version, sequence, count;
    struct gt_contact contacts[GT_CONTACTS];
};
#ifdef __cplusplus
static_assert(sizeof(struct gt_frame) == 136, "touch wire layout");
#else
_Static_assert(sizeof(struct gt_frame) == 136, "touch wire layout");
#endif
static inline int gt_valid(const struct gt_frame *f) {
    if (f->magic != GT_MAGIC || f->version != GT_VERSION || !f->sequence || f->count > GT_CONTACTS) return 0;
    for (unsigned i = 0; i < GT_CONTACTS; ++i) {
        const struct gt_contact *c = &f->contacts[i];
        if (i >= f->count) { if (c->id || c->x || c->y) return 0; continue; }
        if (c->id > 255 || c->x > 32767 || c->y > 32767) return 0;
        for (unsigned j = 0; j < i; ++j) if (f->contacts[j].id == c->id) return 0;
    }
    return 1;
}
#endif
