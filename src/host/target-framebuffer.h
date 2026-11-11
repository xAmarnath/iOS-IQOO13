/* A dedicated shared scanout buffer. Never alias or inspect lent private RAM.
 * Single CPU: read after RUN returns. SMP: the controller alone publishes
 * from shared pixels, which guest CPUs may concurrently update. */
#ifndef TARGET_FRAMEBUFFER_H
#define TARGET_FRAMEBUFFER_H
#include "../viewer/scanout-client.h"
#define FB_GPA UINT64_C(0x15000000)
#define FB_WIDTH 1536u
#define FB_HEIGHT 2048u
#define FB_STRIDE (FB_WIDTH * 4u)
#define FB_SIZE (FB_STRIDE * FB_HEIGHT)
#define FB_CAPTURE_INTERVAL 0.25
_Static_assert(FB_GPA >= SHARED_GPA + SHARED_SIZE, "Framebuffer overlaps disk DMA");
_Static_assert(FB_GPA + FB_SIZE <= UINT64_C(0x3ffd0000), "Framebuffer overlaps GIC");
_Static_assert(FB_GPA + FB_SIZE <= GPA, "Framebuffer overlaps private RAM");
_Static_assert(FB_SIZE % 65536 == 0, "Framebuffer must cover whole host pages");

struct target_framebuffer {
    unsigned char *pixels;
    int directory;
    unsigned generation;
    uint64_t hash;
    double last_capture;
    struct scanout_client stream;
    int stream_initialized;
};

static int framebuffer_prepare(struct target_framebuffer *fb, const char *dtb) {
    fb->stream.socket = -1;
    fb->stream.previous = -1;
    fb->stream_initialized = 1;
    /* The runner passes a DTB within its unique, owned run directory. */
    const char *slash = strrchr(dtb, '/');
    if (!slash || slash == dtb) { errno = EINVAL; return -1; }
    char *parent = strndup(dtb, (size_t)(slash - dtb));
    if (!parent) return -1;
    fb->directory = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    free(parent);
    if (fb->directory < 0) return -1;
    fb->pixels = mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fb->pixels == MAP_FAILED) return -1;
    if (madvise(fb->pixels, FB_SIZE, MADV_NOHUGEPAGE) || mlock(fb->pixels, FB_SIZE)) return -1;
    memset(fb->pixels, 0, FB_SIZE);
    return 0;
}

/* Optional app-owned publication ring. Never expose guest mappings to the app. */
static void framebuffer_stream(struct target_framebuffer *fb, double now) {
    if (!fb->stream_initialized || fb->directory < 0 || !fb->pixels || fb->pixels == MAP_FAILED) return;
    /* scanout_send owns frame pacing. A second gate here rejected alternate
     * frames even when both nominal limits were 120 Hz. */
    struct scanout_client *c = &fb->stream;
    if (c->socket < 0 && now - c->attempted >= 1.0) {
        c->attempted = now;
        int fd = openat(fb->directory, "frame-stream.endpoint", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        if (fd >= 0) {
            struct stat st;
            char text[128], endpoint[96], extra;
            unsigned uid;
            if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
                !(st.st_mode & 0077) && st.st_size > 0 && st.st_size < (off_t)sizeof(text)) {
                ssize_t n = read(fd, text, sizeof(text) - 1);
                if (n == st.st_size) {
                    text[n] = 0;
                    if (sscanf(text, "%95s %u %c", endpoint, &uid, &extra) == 2 && uid >= 10000 && uid <= 1000000)
                        (void)scanout_connect(c, endpoint, uid, now);
                }
            }
            close(fd);
        }
    }
    if (scanout_receive(c, now)) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        (void)scanout_send(c, fb->pixels, now);
    }
}

static int framebuffer_capture(struct target_framebuffer *fb, double now, int force) {
    if (!fb->pixels || fb->pixels == MAP_FAILED || fb->directory < 0 ||
        (!force && now - fb->last_capture < (fb->stream.ring ? 5.0 : FB_CAPTURE_INTERVAL))) return 0;
    fb->last_capture = now;
    /* Ordering only: SMP guests can write concurrently; this is not an
     * atomic frame snapshot or a display-completion fence. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t visible = 0;
    for (size_t i = 0; i < FB_SIZE; i += 4) {
        uint32_t pixel;
        memcpy(&pixel, fb->pixels + i, sizeof(pixel));
        if (pixel & UINT32_C(0xffffff)) ++visible;
        hash = (hash ^ pixel) * UINT64_C(1099511628211);
    }
    if (fb->generation && hash == fb->hash) return 0;
    int fd = openat(fb->directory, "frame-latest.bgra.partial",
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    size_t written = 0;
    while (written < FB_SIZE) {
        ssize_t n = write(fd, fb->pixels + written, FB_SIZE - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        written += (size_t)n;
    }
    int closed = close(fd);
    if (written != FB_SIZE || closed ||
        renameat(fb->directory, "frame-latest.bgra.partial", fb->directory, "frame-latest.bgra")) {
        int saved = errno ? errno : EIO;
        unlinkat(fb->directory, "frame-latest.bgra.partial", 0);
        errno = saved;
        return -1;
    }
    fb->hash = hash;
    ++fb->generation;
    printf("\nFRAMEBUFFER_CAPTURE generation=%u width=%u height=%u stride=%u rgb_nonzero=%zu hash=%016" PRIx64 "\n",
           fb->generation, FB_WIDTH, FB_HEIGHT, FB_STRIDE, visible, hash);
    return 0;
}

/* Call only before start or after terminal quiescence AND VM descriptor close. */
static int framebuffer_release(struct target_framebuffer *fb) {
    int rc = 0;
    if (fb->stream_initialized) scanout_disconnect(&fb->stream);
    if (fb->pixels && fb->pixels != MAP_FAILED) {
        rc = munmap(fb->pixels, FB_SIZE);
        if (!rc) fb->pixels = MAP_FAILED;
    }
    if (fb->directory >= 0) { if (close(fb->directory)) rc = -1; fb->directory = -1; }
    return rc;
}
#endif
