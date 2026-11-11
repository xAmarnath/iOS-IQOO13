#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#define IN
#define OUT
#define EFIAPI
#define VOID void
#define UINTN uintptr_t
#define EFI_STATUS int
#define EFI_PHYSICAL_ADDRESS uintptr_t
#define VIRTIO_DEVICE_PROTOCOL void
#define MAX_UINTN UINTPTR_MAX
#define EFI_PAGE_SIZE 4096u
#define EFI_SIZE_TO_PAGES(n) (((n) + 4095) / 4096)
#define EFI_PAGES_TO_SIZE(n) ((n) * 4096)
#define EFI_SUCCESS 0
#define EFI_NOT_FOUND 1
#define EFI_OUT_OF_RESOURCES 2
#define EFI_INVALID_PARAMETER 3
#define EFI_UNSUPPORTED 4
#define EFI_ERROR(s) ((s) != 0)
#define CopyMem memcpy
#define ZeroMem(p,n) memset((p),0,(n))
#define MemoryFence() atomic_thread_fence(memory_order_seq_cst)
#define AllocateZeroPool(n) calloc(1,(n))
#define FreePool free
typedef enum { VirtioOperationBusMasterRead, VirtioOperationBusMasterWrite,
               VirtioOperationBusMasterCommonBuffer } VIRTIO_MAP_OPERATION;
static _Alignas(4096) unsigned char shared[32768];
static int init_status;
static bool allocated;
static bool exhaust;
static int GunyahDmaPoolInit(void) { return init_status; }
static bool GunyahDmaPoolContains(void *p, size_t n) {
    uintptr_t addr = (uintptr_t)p, base = (uintptr_t)shared;
    return addr >= base && addr - base <= sizeof(shared) &&
           n <= sizeof(shared) - (addr - base);
}
static int GunyahDmaPoolAllocatePages(size_t n, void **p) {
    if (exhaust || allocated || n > sizeof(shared) / 4096) return EFI_OUT_OF_RESOURCES;
    allocated = true;
    *p = shared;
    return 0;
}
static void GunyahDmaPoolFreePages(size_t n, void *p) {
    assert(allocated && p == shared && n <= sizeof(shared) / 4096);
    allocated = false;
}

#include "dma-under-test.inc"

int main(void) {
    unsigned char private[6000], expected[6000];
    for (size_t i = 0; i < sizeof(private); i++) expected[i] = (unsigned char)(i * 31);
    memcpy(private, expected, sizeof(private));
    uintptr_t address;
    void *mapping;
    size_t bytes = sizeof(private);
    assert(VirtioMmioMapSharedBuffer(NULL, VirtioOperationBusMasterRead,
           private, &bytes, &address, &mapping) == 0);
    assert(address == (uintptr_t)shared && mapping && allocated);
    assert(memcmp(shared, expected, sizeof(expected)) == 0);
    for (size_t i = bytes; i < 8192; i++) assert(shared[i] == 0);
    memset(shared, 0xab, bytes);
    assert(VirtioMmioUnmapSharedBuffer(NULL, mapping) == 0);
    assert(!allocated && memcmp(private, expected, bytes) == 0);
    for (size_t i = 0; i < 8192; i++) assert(shared[i] == 0);

    assert(VirtioMmioMapSharedBuffer(NULL, VirtioOperationBusMasterWrite,
           private, &bytes, &address, &mapping) == 0);
    memset((void *)address, 0xa7, bytes);
    assert(memcmp(private, expected, bytes) == 0);
    assert(VirtioMmioUnmapSharedBuffer(NULL, mapping) == 0);
    for (size_t i = 0; i < bytes; i++) assert(private[i] == 0xa7);
    assert(!allocated);

    assert(VirtioMmioMapSharedBuffer(NULL, VirtioOperationBusMasterCommonBuffer,
           private, &bytes, &address, &mapping) == EFI_UNSUPPORTED);
    assert(!mapping && !allocated);
    assert(VirtioMmioMapSharedBuffer(NULL, VirtioOperationBusMasterCommonBuffer,
           shared + 7, &bytes, &address, &mapping) == 0);
    assert(address == (uintptr_t)(shared + 7) && !mapping && !allocated);
    assert(VirtioMmioUnmapSharedBuffer(NULL, NULL) == 0);

    init_status = EFI_OUT_OF_RESOURCES;
    assert(VirtioMmioMapSharedBuffer(NULL, VirtioOperationBusMasterRead,
           private, &bytes, &address, &mapping) == EFI_OUT_OF_RESOURCES);
    assert(address == 0 && !mapping);
    init_status = EFI_NOT_FOUND;
    assert(VirtioMmioMapSharedBuffer(NULL, VirtioOperationBusMasterRead,
           private, &bytes, &address, &mapping) == 0);
    assert(address == (uintptr_t)private && !mapping);
    init_status = EFI_SUCCESS;
    exhaust = true;
    assert(VirtioMmioMapSharedBuffer(NULL, VirtioOperationBusMasterRead,
           private, &bytes, &address, &mapping) == EFI_OUT_OF_RESOURCES);
    assert(!allocated && !mapping && address == 0);
    exhaust = false;
    bytes = UINTPTR_MAX;
    assert(VirtioMmioMapSharedBuffer(NULL, VirtioOperationBusMasterRead,
           private, &bytes, &address, &mapping) == EFI_INVALID_PARAMETER);
    bytes = 0;
    assert(VirtioMmioMapSharedBuffer(NULL, VirtioOperationBusMasterRead,
           private, &bytes, &address, &mapping) == EFI_INVALID_PARAMETER);
    puts("PASS: DMA directions, deferred copy-back, common buffers, page scrubbing, allocation failures, overflow");
    return 0;
}
