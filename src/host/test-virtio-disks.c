#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static volatile sig_atomic_t expired;
#include "virtio-disks.h"

static void reg_write(struct block_device *d, unsigned off, uint32_t value) {
    assert(block_mmio(d, d->mmio_base + off, 4, (unsigned char *)&value, 1) == 0);
}
static uint32_t reg_read(struct block_device *d, unsigned off) {
    uint32_t value = 0;
    assert(block_mmio(d, d->mmio_base + off, 4, (unsigned char *)&value, 0) == 0);
    return value;
}
static void attach(struct block_device *d, unsigned offset) {
    reg_write(d, 0x70, 0);
    reg_write(d, 0x70, 3);
    reg_write(d, 0x24, 0);
    reg_write(d, 0x20, (uint32_t)block_features(d));
    reg_write(d, 0x24, 1);
    reg_write(d, 0x20, 1);
    reg_write(d, 0x70, 11);
    assert(reg_read(d, 0x70) == 11);
    memset(shared + offset, 0, 4096);
    reg_write(d, 0x30, 0);
    reg_write(d, 0x38, 8);
    reg_write(d, 0x80, SHARED_GPA + offset);
    reg_write(d, 0x90, SHARED_GPA + offset + 128);
    reg_write(d, 0xa0, SHARED_GPA + offset + 256);
    reg_write(d, 0x44, 1);
    reg_write(d, 0x70, 15);
}
static uint8_t request(struct block_device *d, uint32_t type, uint64_t sector) {
    uint64_t q = d->desc_gpa;
    unsigned char *p = dma(q, 4096);
    struct descriptor descs[3] = {
        {q + 512, 16, 1, type == 4 ? 2 : 1},
        {q + 4096, 512, type == 0 ? 3 : 1, 2},
        {q + 528, 1, 2, 0},
    };
    memcpy(p, descs, sizeof(descs));
    memcpy(p + 512, &type, 4);
    memcpy(p + 520, &sector, 8);
    p[528] = 0xff;
    uint16_t index = d->last_avail + 1, head = 0;
    memcpy(p + 132 + 2 * (d->last_avail % 8), &head, 2);
    memcpy(p + 130, &index, 2);
    reg_write(d, 0x50, 0);
    uint32_t len;
    memcpy(&len, p + 264 + 8 * ((uint16_t)(d->used_index - 1) % 8), 4);
    uint16_t published;
    memcpy(&published, p + 258, 2);
    assert(published == index && d->last_avail == index && d->used_index == index);
    assert(p[528] || len == (type == 0 ? 513u : 1u));
    return p[528];
}

int main(void) {
    char path[] = "/tmp/ipados-disk-test-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && ftruncate(fd, 4096) == 0);
    unsigned char expected[512], actual[512];
    memset(expected, 0xa5, sizeof(expected));
    assert(pwrite(fd, expected, 512, 0) == 512);
    close(fd);
    shared = mmap(NULL, SHARED_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(shared != MAP_FAILED);
    struct block_device ro, rw, invalid;
    assert(block_open(&ro, path, BLOCK_GPA, 0) == 0);
    assert(block_open(&rw, path, BLOCK_GPA + 0x1000, 1) == 0);
    assert(block_open(&invalid, "/dev/null", BLOCK_GPA, 1) == -1);
    char linkpath[128];
    snprintf(linkpath, sizeof(linkpath), "%s-link", path);
    assert(symlink(path, linkpath) == 0);
    assert(block_open(&invalid, linkpath, BLOCK_GPA, 1) == -1);
    unlink(linkpath);
    attach(&ro, 0x80000);
    attach(&rw, 0xa0000);
    assert(request(&ro, 0, 0) == 0);
    assert(!memcmp(dma(ro.desc_gpa + 4096, 512), expected, 512));
    assert(request(&ro, 1, 0) == 1);
    assert(request(&ro, 0, UINT64_MAX) == 1);
    assert(request(&ro, 0, 8) == 1);
    assert(request(&ro, 0, 7) == 0);
    assert(request(&ro, 99, 0) == 2);
    memset(dma(rw.desc_gpa + 4096, 512), 0x5a, 512);
    assert(request(&rw, 1, 2) == 0);
    assert(pread(rw.disk_fd, actual, 512, 1024) == 512);
    for (unsigned i = 0; i < 512; ++i) assert(actual[i] == 0x5a);
    assert(request(&rw, 4, 0) == 0);
    assert(rw.disk_writes == 1 && rw.disk_flushes == 1 && ro.disk_writes == 0);
    assert(ro.last_avail == 6 && rw.last_avail == 2);
    /* Pass five 16-bit wraps, beyond the last phone failure at 310092 reads. */
    for (unsigned i = 0; i < 350000; ++i) assert(request(&rw, 0, 2) == 0);
    assert(rw.disk_reads == 350000 && rw.completions == 350002);
    uint16_t before_noop = rw.used_index;
    reg_write(&rw, 0x50, 0);
    assert(rw.used_index == before_noop && rw.empty_notifications == 1);
    assert(rw.notifications == rw.completions + 1 && rw.io_errors == 0);
    uint16_t old_index = ro.last_avail;
    reg_write(&rw, 0x30, 9);
    reg_write(&rw, 0x24, 1);
    reg_write(&rw, 0x70, 0);
    assert(!rw.queue_sel && !rw.driver_sel && !rw.last_avail && ro.last_avail == old_index);
    attach(&rw, 0xa0000);
    assert(request(&rw, 0, 2) == 0);
    /* Queue outside DMA and an overfilled ring must fail without host access. */
    rw.desc_gpa = 0x80000000;
    assert(block_notify(&rw) == -1);
    attach(&rw, 0xa0000);
    uint16_t overflow = 9;
    memcpy(dma(rw.avail_gpa + 2, 2), &overflow, 2);
    assert(block_notify(&rw) == -1);
    assert(dma(SHARED_GPA - 1, 1) == NULL);
    assert(dma(SHARED_GPA + SHARED_SIZE, 1) == NULL);
    assert(dma(UINT64_MAX, 512) == NULL);
    close(ro.disk_fd);
    close(rw.disk_fd);
    munmap(shared, SHARED_SIZE);
    unlink(path);
    puts("MULTIDISK_HOST_PASS read write flush readonly bounds isolation reset 350000_reads five_16bit_wraps noop_notify bad_dma regular_file");
}
