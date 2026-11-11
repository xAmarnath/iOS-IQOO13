/* Host protocol tests. The callback is deliberately a test double; actual
 * renderer qualification lives in the Android transport smoke executable. */
#include "vgpu-queue.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE UINT64_C(0x22000000)
#define SIZE (2u << 20)
static unsigned calls, resets;
static unsigned char *ram;
static struct vgpu_device device;
static size_t dispatch(void *opaque, unsigned queue, const void *input, size_t size, void *out, size_t capacity) {
    assert(opaque == ram && queue == 0 && size >= 24 && capacity >= 24);
    struct vgpu_header request; memcpy(&request, input, 24);
    assert(request.type == 0x108);
    /* Changing the guest backing while dispatching must not change the copy. */
    memset(ram + 0x10000, 0xa7, size);
    assert(((const struct vgpu_header *)input)->type == request.type);
    struct vgpu_header response = {.type = 0x1102};
    memcpy(out, &response, 24); calls++; return 24;
}
static void reset(void *opaque) { assert(opaque == ram); resets++; }
static int wr(uint32_t offset, uint32_t value) { return vgpu_mmio(&device, offset, 4, 1, &value); }
static uint32_t rd(uint32_t offset) { uint32_t v = 0; assert(!vgpu_mmio(&device, offset, 4, 0, &v)); return v; }
static void setup(void) {
    assert(!wr(0x70, 0)); memset(ram, 0, SIZE); calls = 0;
    assert(!wr(0x70, 3)); assert(!wr(0x20, (uint32_t)VGPU_FEATURES));
    assert(!wr(0x24, 1)); assert(!wr(0x20, 1)); assert(!wr(0x70, 11));
    assert(!wr(0x38, 512)); assert(!wr(0x80, BASE));
    assert(!wr(0x90, BASE + 8192)); assert(!wr(0xa0, BASE + 10240));
    assert(!wr(0x44, 1)); assert(!wr(0x70, 15));
    struct vgpu_header h = {.type = 0x108}; memcpy(ram + 0x10000, &h, sizeof(h));
    struct vgpu_desc desc[2] = {{BASE + 0x10000, 32, 1, 1}, {BASE + 0x20000, 64, 2, 0}};
    memcpy(ram, desc, sizeof(desc));
    uint16_t avail[] = {1, 1, 0}; memcpy(ram + 8192, avail, sizeof(avail));
}
static void reject(void) {
    assert(wr(0x50, 0) < 0); assert(!calls && (rd(0x70) & 64));
    uint16_t used; memcpy(&used, ram + 10242, 2); assert(!used);
}
int main(void) {
    ram = calloc(1, SIZE); assert(ram);
    assert(!vgpu_init(&device, ram, BASE, SIZE, dispatch, reset, ram));
    assert(rd(0) == 0x74726976 && rd(8) == 16 && rd(0x108) == 0 && rd(0x10c) == 1);
    /* Old firmware GOP wrongly bound this headless device, then tried to
     * enable a ring in the firmware pool. Keep that DMA rejection intact. */
    assert(!wr(0x70, 3) && !wr(0x24, 1) && !wr(0x20, 1) && !wr(0x70, 11));
    assert(!wr(0x38, 512) && !wr(0x80, 0x20000000));
    assert(!wr(0x90, 0x20002000) && !wr(0xa0, 0x20003000));
    assert(wr(0x44, 1) < 0 && !device.queue[0].ready && !calls);
    assert(device.errors == 1 && (rd(0x70) & 64));
    assert(vgpu_dma(&device, BASE, SIZE) == ram);
    assert(!vgpu_dma(&device, BASE, 0) && !vgpu_dma(&device, BASE - 1, 1));
    assert(!vgpu_dma(&device, BASE + SIZE, 1) && !vgpu_dma(&device, UINT64_MAX, 2));
    assert(!vgpu_dma(&device, UINT64_C(0x80000000), 32));
    setup(); assert(!wr(0x50, 0) && calls == 1);
    assert(((struct vgpu_header *)(ram + 0x20000))->type == 0x1102);
    uint32_t used[2]; memcpy(used, ram + 10244, 8); assert(used[0] == 0 && used[1] == 24);
    assert(!wr(0x50, 0) && calls == 1); /* empty notify cannot duplicate work */
    assert(!rd(0x60));
    setup(); ((uint16_t *)(ram + 8192))[0] = 0;
    assert(!wr(0x50, 0) && rd(0x60) == 1); assert(!wr(0x64, 1) && !rd(0x60));
    setup(); ((struct vgpu_desc *)ram)[0].address = 0x80000000; reject();
    setup(); ((struct vgpu_desc *)ram)[1].address = UINT64_MAX - 12; reject();
    setup(); ((struct vgpu_desc *)ram)[0].flags |= 4; reject();
    setup(); ((struct vgpu_desc *)ram)[0].next = 0; reject();
    setup(); ((struct vgpu_desc *)ram)[0].next = 512; reject();
    setup(); ((struct vgpu_desc *)ram)[1].address = BASE + 0x10000; reject();
    setup(); ((struct vgpu_desc *)ram)[1].address = BASE + 10240; reject();
    setup(); ((struct vgpu_desc *)ram)[0].length = VGPU_REQUEST_MAX + 1; reject();
    setup(); ((struct vgpu_desc *)ram)[1].length = VGPU_REPLY_MAX + 1; reject();
    setup(); ((struct vgpu_desc *)ram)[0].length = 0; reject();
    setup(); ((struct vgpu_desc *)ram)[1].length = 23; reject();
    setup(); ((struct vgpu_desc *)ram)[0].length = 23; reject();
    setup(); ((struct vgpu_desc *)ram)[1].flags = 0; reject();
    setup(); ((uint16_t *)(ram + 8192))[1] = 513; reject();
    setup(); ((uint16_t *)(ram + 8192))[2] = 512; reject();
    setup(); ((struct vgpu_header *)(ram + 0x10000))->flags = 1;
    assert(!wr(0x50, 0) && !calls);
    assert(((struct vgpu_header *)(ram + 0x20000))->type == VGPU_RESP_INVALID);
    setup(); assert(wr(0x80, BASE + 16) < 0); /* live queue mutation */
    setup(); assert(!wr(0x44, 0)); assert(!wr(0x90, BASE)); assert(wr(0x44, 1) < 0);
    setup(); assert(!wr(0x30, 1)); assert(!wr(0x38, 4)); assert(!wr(0x80, BASE));
    assert(!wr(0x90, BASE + 0x8000)); assert(!wr(0xa0, BASE + 0x9000)); assert(wr(0x44, 1) < 0);
    setup(); assert(!wr(0x70, 0)); assert(!wr(0x70, 3)); assert(wr(0x70, 11) < 0);
    setup(); assert(wr(0x20, 0) < 0); /* negotiated feature immutability */
    setup(); uint32_t value = 0; assert(vgpu_mmio(&device, 0, 8, 0, &value) < 0);
    /* Split buffers and wrap through the 16-bit producer sequence. */
    setup(); device.queue[0].consumed = device.queue[0].produced = 65535;
    ((uint16_t *)(ram + 8192))[1] = 0; ((uint16_t *)(ram + 8192))[2 + 511] = 0;
    struct vgpu_desc split[] = {{BASE + 0x10000, 12, 1, 1}, {BASE + 0x1000c, 20, 1, 2},
                               {BASE + 0x20000, 12, 3, 3}, {BASE + 0x2000c, 12, 2, 0}};
    memcpy(ram, split, sizeof(split)); assert(!wr(0x50, 0) && calls == 1 && !device.queue[0].produced);
    /* Deterministic malformed-descriptor fuzz: bounded execution and ASAN/UBSAN. */
    uint32_t random = 0x830;
    for (unsigned i = 0; i < 20000; i++) {
        setup(); unsigned char *p = ram;
        for (unsigned j = 0; j < 32; j++) { random ^= random << 13; random ^= random >> 17; random ^= random << 5; p[j] = random; }
        reject();
    }
    vgpu_destroy(&device); free(ram);
    assert(resets > 20000);
    puts("VGPU_QUEUE_BOUNDARIES_OK fuzz=20000 private_dma_rejected=1 wrap=1 copied_request=1");
}
