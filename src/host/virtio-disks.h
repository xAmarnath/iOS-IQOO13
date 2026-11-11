/* Polling modern virtio-blk transport for iPadOS bring-up.
 * Separate state per disk; shared DMA only; regular-file backing only.
 * OS/boot images are read-only; only the explicitly selected Data file is writable.
 * Host tested independently; not yet qualified on the phone. */
#include <sys/stat.h>

#define SHARED_GPA UINT64_C(0x14000000)
/* Match the 1 MiB shared mapping already exercised by the PAC probe. */
#define SHARED_SIZE (1u * 1024u * 1024u)
#define BLOCK_GPA UINT64_C(0x10001000)
#define QUEUE_MAX 128u
static unsigned char *shared = MAP_FAILED;
struct block_device {
    int disk_fd;
    uint64_t disk_size, mmio_base;
    int writable;
    unsigned long disk_reads, disk_writes, disk_flushes;
    unsigned long notifications, completions, empty_notifications, resets, io_errors;
    uint32_t last_type;
    uint64_t last_sector;
    uint32_t feature_sel, driver_sel, queue_sel, queue_num, queue_ready;
    uint32_t device_status, irq_status;
    uint64_t driver_features, desc_gpa, avail_gpa, used_gpa;
    uint16_t last_avail, used_index;
};

static void block_report(const struct block_device *d, const char *event) {
    fprintf(stderr, "BLOCK_QUEUE event=%s base=0x%" PRIx64
            " notify=%lu complete=%lu empty=%lu reset=%lu errors=%lu"
            " avail=%u used=%u ready=%u status=0x%x type=%u sector=%" PRIu64 "\n",
            event, d->mmio_base, d->notifications, d->completions,
            d->empty_notifications, d->resets, d->io_errors,
            d->last_avail, d->used_index, d->queue_ready, d->device_status,
            d->last_type, d->last_sector);
}

static uint64_t block_features(const struct block_device *d) {
    return (UINT64_C(1) << 32) | (d->writable ? (1u << 9) : (1u << 5));
}

/* Caller selects the path and write permission; never creates or resizes files. */
static int block_open(struct block_device *d, const char *path, uint64_t base, int writable) {
    memset(d, 0, sizeof(*d));
    d->disk_fd = -1;
    int fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size % 512) {
        close(fd);
        return -1;
    }
    d->disk_fd = fd;
    d->disk_size = (uint64_t)st.st_size;
    d->mmio_base = base;
    d->writable = writable;
    return 0;
}

static void *dma(uint64_t address, size_t size) {
    if (address < SHARED_GPA || size > SHARED_SIZE ||
        address - SHARED_GPA > SHARED_SIZE - size) {
        fprintf(stderr, "DMA outside shared RAM: address=0x%" PRIx64 " length=%zu\n", address, size);
        return NULL;
    }
    return shared + (address - SHARED_GPA);
}

struct descriptor { uint64_t addr; uint32_t len; uint16_t flags, next; };
_Static_assert(sizeof(struct descriptor) == 16, "descriptor layout");

static int block_notify(struct block_device *d) {
    ++d->notifications;
    if (!d->queue_ready || !(d->device_status & 4) || !d->queue_num ||
        d->queue_num > QUEUE_MAX || (d->queue_num & (d->queue_num - 1))) return -1;
    unsigned char *descs = dma(d->desc_gpa, d->queue_num * 16);
    unsigned char *avail = dma(d->avail_gpa, 6 + d->queue_num * 2);
    unsigned char *used = dma(d->used_gpa, 6 + d->queue_num * 8);
    if (!descs || !avail || !used) return -1;
    uint16_t new_avail;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    memcpy(&new_avail, avail + 2, 2);
    if (new_avail == d->last_avail) ++d->empty_notifications;
    if ((uint16_t)(new_avail - d->last_avail) > d->queue_num) return -1;
    while (d->last_avail != new_avail) {
        uint16_t head, index;
        memcpy(&head, avail + 4 + 2 * (d->last_avail % d->queue_num), 2);
        index = head;
        struct descriptor chain[QUEUE_MAX];
        unsigned count = 0;
        for (;;) {
            if (index >= d->queue_num || count == d->queue_num) return -1;
            memcpy(&chain[count], descs + 16 * index, 16);
            struct descriptor *d = &chain[count++];
            if (d->flags & ~3u || !dma(d->addr, d->len)) return -1;
            if (!(d->flags & 1)) break;
            index = d->next;
        }
        if (count < 2 || chain[0].len < 16 || (chain[0].flags & 2) ||
            !(chain[count-1].flags & 2) || chain[count-1].len != 1) return -1;
        unsigned char *header = dma(chain[0].addr, 16);
        uint32_t type;
        uint64_t sector;
        memcpy(&type, header, 4);
        memcpy(&sector, header + 8, 8);
        d->last_type = type;
        d->last_sector = sector;
        uint8_t status = 2; /* VIRTIO_BLK_S_UNSUPP */
        uint32_t written = 1;
        if (type == 0 || (type == 1 && d->writable)) {
            uint64_t total = 0;
            for (unsigned i=1; i+1<count; i++) {
                if (!!(chain[i].flags & 2) != (type == 0)) return -1;
                total += chain[i].len;
            }
            status = 1;
            if (total <= SHARED_SIZE && total % 512 == 0 &&
                sector <= d->disk_size / 512 && total <= d->disk_size - sector * 512) {
                uint64_t offset = sector * 512;
                status = 0;
                for (unsigned i=1; i+1<count; i++) {
                    unsigned char *buffer = dma(chain[i].addr, chain[i].len);
                    size_t done = 0;
                    while (done < chain[i].len) {
                        ssize_t n = type == 0 ? pread(d->disk_fd, buffer + done, chain[i].len - done, offset + done)
                                              : pwrite(d->disk_fd, buffer + done, chain[i].len - done, offset + done);
                        if (n < 0 && errno == EINTR && !expired) continue;
                        if (n <= 0) { status = 1; break; }
                        done += (size_t)n;
                    }
                    if (type == 0) written += (uint32_t)done;
                    if (status) break;
                    offset += chain[i].len;
                }
                if (!status) {
                    if (type == 0) ++d->disk_reads;
                    else ++d->disk_writes;
                }
            }
        }
        if (type == 1 && !d->writable) status = 1; /* read-only */
        if (type == 4 && d->writable) {
            if (count != 2) return -1;
            int rc;
            do { rc = fdatasync(d->disk_fd); } while (rc < 0 && errno == EINTR && !expired);
            status = rc == 0 ? 0 : 1;
            if (!status) ++d->disk_flushes;
        }
        *(uint8_t *)dma(chain[count-1].addr, 1) = status;
        if (status) {
            ++d->io_errors;
            block_report(d, "request_error");
        }
        uint32_t id = head;
        memcpy(used + 4 + 8 * (d->used_index % d->queue_num), &id, 4);
        memcpy(used + 8 + 8 * (d->used_index % d->queue_num), &written, 4);
        ++d->used_index;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        memcpy(used + 2, &d->used_index, 2);
        ++d->last_avail;
        ++d->completions;
        d->irq_status = 1; /* Firmware polls the ring; no virtual interrupt needed. */
    }
    return 0;
}

static int block_mmio(struct block_device *d, uint64_t address, unsigned length, unsigned char *data, int write) {
    if (address < d->mmio_base || address - d->mmio_base >= 0x1000) return -1;
    unsigned off = (unsigned)(address - d->mmio_base);
    uint32_t value = 0;
    if (off >= 0x100 && off + length <= 0x140 && (length == 1 || length == 2 || length == 4 || length == 8)) {
        unsigned char config[64] = {0};
        uint64_t sectors = d->disk_size / 512;
        uint32_t block_size = 512;
        memcpy(config, &sectors, 8);
        memcpy(config + 20, &block_size, 4);
        if (write) return -1;
        memcpy(data, config + off - 0x100, length);
        return 0;
    }
    if (length != 4 || off % 4) return -1;
    if (!write) {
        switch (off) {
        case 0x00: value=0x74726976; break;
        case 0x04: value=2; break;
        case 0x08: value=2; break;
        case 0x0c: value=0x514d5545; break;
        case 0x10: value=d->feature_sel < 2 ? (uint32_t)(block_features(d) >> (d->feature_sel * 32)) : 0; break;
        case 0x34: value=d->queue_sel == 0 ? QUEUE_MAX : 0; break;
        case 0x44: value=d->queue_sel == 0 ? d->queue_ready : 0; break;
        case 0x60: value=d->irq_status; break;
        case 0x70: value=d->device_status; break;
        case 0xfc: value=0; break;
        default: fprintf(stderr, "Unsupported virtio read 0x%x\n", off); return -1;
        }
        memcpy(data, &value, 4);
        return 0;
    }
    memcpy(&value, data, 4);
    switch (off) {
    case 0x14: d->feature_sel=value; break;
    case 0x20:
        if (d->driver_sel > 1) return -1;
        d->driver_features = (d->driver_features & ~(UINT64_C(0xffffffff) << (d->driver_sel * 32))) | ((uint64_t)value << (d->driver_sel * 32));
        break;
    case 0x24: d->driver_sel=value; break;
    case 0x30: d->queue_sel=value; break;
    case 0x38: if (d->queue_sel) return -1; d->queue_num=value; break;
    case 0x44: if (d->queue_sel || value > 1) return -1; d->queue_ready=value; break;
    case 0x50: if (value) return -1; return block_notify(d);
    case 0x64: d->irq_status &= ~value; break;
    case 0x70:
        if (!value) {
            ++d->resets;
            block_report(d, "reset");
            d->device_status=d->queue_ready=d->queue_num=0;
            d->last_avail=d->used_index=0;
            d->driver_features=d->desc_gpa=d->avail_gpa=d->used_gpa=0;
            d->irq_status=0;
            d->feature_sel=d->driver_sel=d->queue_sel=0;
        } else {
            d->device_status=value;
            if ((value & 8) && ((d->driver_features & ~block_features(d)) || !(d->driver_features & (UINT64_C(1) << 32)))) d->device_status &= ~8u;
        }
        break;
    case 0x80: d->desc_gpa=(d->desc_gpa & UINT64_C(0xffffffff00000000)) | value; break;
    case 0x84: d->desc_gpa=(d->desc_gpa & UINT64_C(0xffffffff)) | ((uint64_t)value << 32); break;
    case 0x90: d->avail_gpa=(d->avail_gpa & UINT64_C(0xffffffff00000000)) | value; break;
    case 0x94: d->avail_gpa=(d->avail_gpa & UINT64_C(0xffffffff)) | ((uint64_t)value << 32); break;
    case 0xa0: d->used_gpa=(d->used_gpa & UINT64_C(0xffffffff00000000)) | value; break;
    case 0xa4: d->used_gpa=(d->used_gpa & UINT64_C(0xffffffff)) | ((uint64_t)value << 32); break;
    default: fprintf(stderr, "Unsupported virtio write 0x%x = 0x%x\n", off, value); return -1;
    }
    return 0;
}
