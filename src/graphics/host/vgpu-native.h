/* First native-context transport stage: capset, contexts and response blobs.
 * GPU command BOs and fence retirement must be implemented before Metal. */
#ifndef OPENOOS_VGPU_NATIVE_H
#define OPENOOS_VGPU_NATIVE_H
#include <stddef.h>
#include <stdint.h>
struct vgpu_native;
/* One arena per backend process. The caller retains fd and mapping until a
 * successful destroy. EBUSY retains native state and all live GPU backing.
 * pool is the mapping of [fd_offset, fd_offset + size), not private VM RAM. */
struct vgpu_native *vgpu_native_create(int fd, void *pool, size_t size, uint64_t fd_offset);
size_t vgpu_native_dispatch(void *, unsigned, const void *, size_t, void *, size_t);
void vgpu_native_reset(void *);
int vgpu_native_destroy(struct vgpu_native *);
#endif
