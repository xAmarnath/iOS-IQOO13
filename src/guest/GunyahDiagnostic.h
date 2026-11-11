/* SPDX-License-Identifier: MIT
 * One outstanding, read-only request. Shared bytes are consumed only across
 * synchronous MMIO exits on the one-vCPU Gunyah target. All fields are LE. */
#ifndef DROIDVM_GUNYAH_DIAGNOSTIC_H
#define DROIDVM_GUNYAH_DIAGNOSTIC_H
#include <stdint.h>
#define GD_MAGIC UINT32_C(0x47444731)
#define GD_VERSION 2u
#define GD_GPA UINT64_C(0x140c4000)
#define GD_SIZE UINT32_C(0x20000)
#define GD_POLL UINT64_C(0x10000020)
#define GD_DONE UINT64_C(0x10000024)
#define GD_PROCESSES 2u
#define GD_MEMORY 4u
#define GD_STACKSHOT 7u
struct gd_request {
    uint32_t magic, version, sequence, kind;
    int32_t pid, status;
    uint64_t offset, length, total;
    uint64_t reserved[2];
};
#ifdef __cplusplus
static_assert(sizeof(struct gd_request) == 64, "diagnostic wire layout");
#else
_Static_assert(sizeof(struct gd_request) == 64, "diagnostic wire layout");
#endif
#define GD_CAPACITY (GD_SIZE - sizeof(struct gd_request))
static inline int gd_request_valid(const struct gd_request *r) {
    if (r->magic != GD_MAGIC || r->version != GD_VERSION || !r->sequence ||
        r->status || r->total || r->reserved[0] || r->reserved[1]) return 0;
    if (r->kind == GD_MEMORY)
        return r->pid > 0 && r->length > 0 && r->length <= GD_CAPACITY &&
               r->offset > 0 && r->offset < (UINT64_C(1) << 47) &&
               r->length <= (UINT64_C(1) << 47) - r->offset;
    if (r->length) return 0;
    if (r->kind == GD_PROCESSES) return r->pid == 0 && r->offset == 0;
    if (r->kind == GD_STACKSHOT)
        return (r->pid == -1 || r->pid > 0) && r->offset < (8u << 20);
    return 0;
}
#endif
