/* Android-only drm2kgsl service. Inherits socket3 and sealed memory4, no VM. */
#include "vgpu-ipc.h"
#include "vgpu-native.h"
#include "vgpu-queue.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    int64_t startup_ms = vgpu_now_ms();
    printf("GPU_BACKEND_START pid=%ld monotonic_ms=%lld\n", (long)getpid(), (long long)startup_ms);
    if (argc != 3 || strcmp(argv[1], "3") || strcmp(argv[2], "4")) return 2;
    int type = 0; socklen_t length = sizeof type;
    struct stat st;
    int seals = fcntl(4, F_GET_SEALS);
    if (getsockopt(3, SOL_SOCKET, SO_TYPE, &type, &length) || type != SOCK_SEQPACKET ||
        fstat(4, &st) || st.st_size != VGPU_MEMORY_SIZE || seals < 0 ||
        (seals & (F_SEAL_GROW | F_SEAL_SHRINK)) != (F_SEAL_GROW | F_SEAL_SHRINK)) return 2;
    unsigned char *shared = mmap(NULL, VGPU_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, 4, 0);
    if (shared == MAP_FAILED) return 2;
    struct vgpu_native *native = vgpu_native_create(4, shared + VGPU_COMMAND_SIZE, VGPU_POOL_SIZE, VGPU_COMMAND_SIZE);
    int native_errno = errno;
    printf("GPU_BACKEND_NATIVE_READY elapsed_ms=%lld success=%d\n",
           (long long)(vgpu_now_ms() - startup_ms), native != NULL);
    struct vgpu_device device;
    if (!native) { errno = native_errno; perror("GPU native create"); munmap(shared, VGPU_MEMORY_SIZE); return 2; }
    if (vgpu_init(&device, shared, VGPU_COMMAND_BASE, VGPU_COMMAND_SIZE,
                  vgpu_native_dispatch, vgpu_native_reset, native)) {
        vgpu_native_destroy(native); munmap(shared, VGPU_MEMORY_SIZE); return 2;
    }
    uint32_t sequence = 0; int result = 1;
    for (;;) {
        /* Idle sessions can remain held indefinitely. EOF is never ignored. */
        struct pollfd p = {.fd = 3, .events = POLLIN};
        int rc = poll(&p, 1, -1);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) break;
        struct vgpu_packet message;
        if (vgpu_packet_recv(3, &message, vgpu_now_ms() + 2000) || sequence == UINT32_MAX ||
            !vgpu_request_valid(&message, ++sequence)) break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (message.operation == VGPU_IPC_MMIO) {
            message.result = vgpu_mmio(&device, message.offset, message.width, message.write, &message.value);
            if (message.result < 0 && device.errors <= 8) {
                const struct vgpu_queue *q = device.queue_sel < 2 ? &device.queue[device.queue_sel] : NULL;
                printf("GPU_MMIO_REJECT seq=%u offset=0x%x width=%u write=%u value=0x%x result=%d "
                       "status=0x%x queue=%u num=%u ready=%u desc=0x%llx avail=0x%llx used=0x%llx\n",
                       sequence, message.offset, message.width, message.write, message.value, message.result,
                       device.status, device.queue_sel, q ? q->num : 0, q ? q->ready : 0,
                       (unsigned long long)(q ? q->desc : 0), (unsigned long long)(q ? q->avail : 0),
                       (unsigned long long)(q ? q->used : 0));
            }
        }
        __atomic_thread_fence(__ATOMIC_RELEASE);
        if (vgpu_packet_send(3, &message, vgpu_now_ms() + 2000)) break;
        if (message.operation == VGPU_IPC_STOP) { result = 0; break; }
    }
    printf("GPU_BACKEND_EXIT result=%d requests=%u completions=%llu errors=%llu\n", result, sequence,
           (unsigned long long)device.completions, (unsigned long long)device.errors);
    vgpu_destroy(&device);
    if (vgpu_native_destroy(native)) {
        /* Do not release mappings, imported pages or KGSL descriptors while
         * native execution is uncertain. A normal phone restart can reclaim
         * this isolated backend. The monitor must retain its shared mapping. */
        printf("GPU_BACKEND_RETAINED pid=%ld reason=unretired_gpu_work\n", (long)getpid());
        close(3);
        for (;;) pause();
    }
    munmap(shared, VGPU_MEMORY_SIZE); close(4); close(3);
    return result;
}
