/* Bounded modern virtio-gpu transport. Only the explicit command SHARE window
 * is addressable here; renderer BOs have a separate allocation contract. */
#ifndef OPENOOS_VGPU_QUEUE_H
#define OPENOOS_VGPU_QUEUE_H
#include <stddef.h>
#include <stdint.h>

#define VGPU_QUEUE_MAX 512u
#define VGPU_REQUEST_MAX (1u << 20)
#define VGPU_REPLY_MAX (128u << 10)
#define VGPU_FEATURES ((UINT64_C(1) << 32) | 1u | 8u | 16u)
#define VGPU_RESP_OK 0x1100u
#define VGPU_RESP_INVALID 0x1205u

struct vgpu_header {
    uint32_t type, flags;
    uint64_t fence;
    uint32_t context;
    uint8_t ring, padding[3];
};
struct vgpu_desc { uint64_t address; uint32_t length; uint16_t flags, next; };
struct vgpu_queue {
    uint64_t desc, avail, used;
    uint32_t num, ready;
    uint16_t consumed, produced;
};
/* Inputs and output are private host copies, never live guest pointers.
 * Return the number of response bytes; zero means a host transport failure. */
typedef size_t (*vgpu_dispatch)(void *, unsigned, const void *, size_t, void *, size_t);
typedef void (*vgpu_reset)(void *);
struct vgpu_device {
    unsigned char *shared, *request, *reply;
    uint64_t base, size, negotiated, completions, errors;
    uint32_t feature_sel, driver_sel, queue_sel, status, irq;
    struct vgpu_queue queue[2];
    vgpu_dispatch dispatch;
    vgpu_reset reset;
    void *opaque;
};
int vgpu_init(struct vgpu_device *, void *, uint64_t, size_t, vgpu_dispatch, vgpu_reset, void *);
void vgpu_destroy(struct vgpu_device *);
void *vgpu_dma(struct vgpu_device *, uint64_t, size_t);
int vgpu_mmio(struct vgpu_device *, uint32_t, unsigned, int, uint32_t *);
int vgpu_notify(struct vgpu_device *, unsigned);
#endif
