/* Read-only guest diagnostics; never modifies VM lifetime or disk state. */
#ifndef TARGET_DIAGNOSTIC_H
#define TARGET_DIAGNOSTIC_H
#include "../guest/GunyahDiagnostic.h"
_Static_assert(GD_GPA >= SHARED_GPA + 0xc4000, "overlaps block DMA");
_Static_assert(GD_GPA + GD_SIZE <= SHARED_GPA + SHARED_SIZE, "outside shared RAM");
static int gd_directory = -1;
static uint32_t gd_completed;
static struct gd_request gd_active;

static int gd_prepare(const char *dtb) {
    const char *slash = strrchr(dtb, '/');
    if (!slash || slash == dtb) { errno = EINVAL; return -1; }
    char *parent = strndup(dtb, (size_t)(slash - dtb));
    if (!parent) return -1;
    gd_directory = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    free(parent);
    return gd_directory < 0 ? -1 : 0;
}

static int gd_transfer(int fd, void *buffer, size_t length, int writing) {
    unsigned char *p = buffer;
    while (length) {
        ssize_t n = writing ? write(fd, p, length) : read(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        p += (size_t)n;
        length -= (size_t)n;
    }
    return 0;
}

static uint32_t gd_poll(void) {
    if (gd_directory < 0 || shared == MAP_FAILED) return 0;
    if (gd_active.sequence) return gd_active.sequence;
    int fd = openat(gd_directory, "diagnostic.request", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return 0;
    struct stat st;
    struct gd_request r;
    int valid = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size == sizeof(r) &&
                !gd_transfer(fd, &r, sizeof(r), 0) && gd_request_valid(&r) && r.sequence > gd_completed;
    close(fd);
    if (!valid) return 0;
    gd_active = r;
    /* No old response bytes can leak through a short or failed guest result. */
    unsigned char *memory = shared + (GD_GPA - SHARED_GPA);
    memset(memory, 0, GD_SIZE);
    memcpy(memory, &r, sizeof(r));
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    printf("GUEST_DIAGNOSTIC_REQUEST sequence=%u kind=%u pid=%d offset=%" PRIu64 "\n",
           r.sequence, r.kind, r.pid, r.offset);
    return r.sequence;
}

static int gd_finish(uint32_t sequence) {
    if (!sequence || sequence != gd_active.sequence || shared == MAP_FAILED || gd_directory < 0) return -1;
    unsigned char *memory = shared + (GD_GPA - SHARED_GPA);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    struct gd_request r;
    memcpy(&r, memory, sizeof(r));
    if (r.magic != GD_MAGIC || r.version != GD_VERSION || r.sequence != sequence ||
        r.kind != gd_active.kind || r.pid != gd_active.pid || r.offset != gd_active.offset ||
        r.reserved[0] || r.reserved[1] || r.status < 0 || r.length > GD_CAPACITY ||
        (r.status && (r.length || r.total)) ||
        (r.kind == GD_MEMORY && !r.status &&
         (r.length != gd_active.length || r.total != r.length)) ||
        (r.kind == GD_STACKSHOT && !r.status &&
         (r.total > (8u << 20) || r.offset > r.total || r.length > r.total - r.offset))) return -1;
    char name[64];
    snprintf(name, sizeof(name), "diagnostic-%08u.response", sequence);
    int fd = openat(gd_directory, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    int error = gd_transfer(fd, &r, sizeof(r), 1) ||
                gd_transfer(fd, memory + sizeof(r), (size_t)r.length, 1) || fsync(fd);
    if (close(fd)) error = 1;
    if (error) return -1; /* Keep outstanding request; never authorize teardown. */
    printf("GUEST_DIAGNOSTIC_DONE sequence=%u status=%d bytes=%" PRIu64 " total=%" PRIu64 "\n",
           sequence, r.status, r.length, r.total);
    gd_completed = sequence;
    memset(&gd_active, 0, sizeof(gd_active));
    return 0;
}

static int gd_mmio(uint64_t address, unsigned length, unsigned char *data, int writing) {
    if (length != 4) return -1;
    uint32_t value;
    if (address == GD_POLL && !writing) {
        value = gd_poll();
        memcpy(data, &value, 4);
        return 0;
    }
    if (address == GD_DONE && writing) {
        memcpy(&value, data, 4);
        /* An export failure must not strand an otherwise healthy guest. */
        if (gd_finish(value)) fprintf(stderr, "GUEST_DIAGNOSTIC_RESPONSE_REJECTED sequence=%u\n", value);
        return 0;
    }
    return -1;
}
#endif
