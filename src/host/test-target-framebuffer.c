/* Exercise real bounds and capture I/O. Host mlock policy is modeled only. */
#include <stddef.h>
static int test_mlock(const void *address, size_t length) { (void)address; (void)length; return 0; }
#define mlock test_mlock
#define main unused_monitor_main
#include "monitor.c"
#undef main
#undef mlock
#include <assert.h>

int main(void) {
    char dir[] = "/tmp/xnu-framebuffer-XXXXXX";
    assert(mkdtemp(dir));
    char dtb[256];
    assert(snprintf(dtb, sizeof(dtb), "%s/guest.dtb", dir) > 0);
    struct target_framebuffer fb = {.pixels = MAP_FAILED, .directory = -1};
    assert(framebuffer_prepare(&fb, "relative.dtb") == -1);
    assert(framebuffer_prepare(&fb, dtb) == 0);
    shared = mmap(NULL, SHARED_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(shared != MAP_FAILED);
    assert(dma(SHARED_GPA + SHARED_SIZE - 1, 1));
    assert(!dma(FB_GPA, 4));
    assert(!dma(FB_GPA + FB_SIZE - 4, 4));
    assert(!dma(GPA, 4));
    assert(framebuffer_capture(&fb, 2.0, 0) == 0 && fb.generation == 1);
    fb.pixels[0] = 0x12; fb.pixels[1] = 0x34; fb.pixels[2] = 0x56;
    fb.pixels[FB_SIZE - 1] = 0xff;
    assert(framebuffer_capture(&fb, 2.249, 0) == 0 && fb.generation == 1);
    assert(framebuffer_capture(&fb, 2.25, 0) == 0 && fb.generation == 2);
    assert(framebuffer_capture(&fb, 6.0, 0) == 0 && fb.generation == 2);
    int fd = openat(fb.directory, "frame-latest.bgra", O_RDONLY | O_NOFOLLOW);
    struct stat st;
    assert(fd >= 0 && fstat(fd, &st) == 0 && st.st_size == FB_SIZE);
    unsigned char pixel[4];
    assert(read(fd, pixel, 4) == 4 && pixel[0] == 0x12 && pixel[1] == 0x34 && pixel[2] == 0x56);
    assert(lseek(fd, FB_SIZE - 1, SEEK_SET) == FB_SIZE - 1 && read(fd, pixel, 1) == 1 && pixel[0] == 0xff);
    assert(close(fd) == 0);
    assert(symlinkat("frame-latest.bgra", fb.directory, "frame-latest.bgra.partial") == 0);
    fb.pixels[0]++;
    assert(framebuffer_capture(&fb, 8.0, 0) == -1 && fb.generation == 2);
    assert(unlinkat(fb.directory, "frame-latest.bgra.partial", 0) == 0);
    assert(framebuffer_capture(&fb, 8.0, 1) == 0 && fb.generation == 3);
    /* Real publication, dedup, context recreation, and legacy fallback. */
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) == 0);
    fb.stream.socket = sockets[0];
    fb.stream.ring = mmap(NULL, SCANOUT_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(fb.stream.ring != MAP_FAILED);
    framebuffer_stream(&fb, 8.01);
    assert(fb.stream.sent == 1);
    int slot = scanout_acquire(fb.stream.ring, 0);
    assert(slot >= 0 && !memcmp(scanout_pixels(fb.stream.ring, (unsigned)slot), fb.pixels, FB_SIZE));
    scanout_release(fb.stream.ring, (unsigned)slot);
    framebuffer_stream(&fb, 8.03);
    assert(fb.stream.sent == 1);
    __atomic_store_n(&fb.stream.ring->reserved, 1, __ATOMIC_RELEASE);
    framebuffer_stream(&fb, 8.05);
    assert(fb.stream.sent == 2);
    /* Exercise both production throttles, with a real consumer and changing
     * pixels. Counting the outer poll alone missed a second cadence filter. */
    fb.stream.last_frame = 20.0;
    uint64_t before = fb.stream.sent;
    for (unsigned tick=1; tick<=250; ++tick) {
        ++fb.pixels[100];
        framebuffer_stream(&fb, 20.0+tick*0.004);
        int newest = scanout_acquire(fb.stream.ring, 0);
        if (newest >= 0) scanout_release(fb.stream.ring, (unsigned)newest);
    }
    printf("SCANOUT_CADENCE published=%llu expected=119..120\n",
           (unsigned long long)(fb.stream.sent-before)); fflush(stdout);
    assert(fb.stream.sent-before >=119 && fb.stream.sent-before <=120);
    /* A long stall permits one new frame, never a backlog of duplicate sends. */
    ++fb.pixels[100]; framebuffer_stream(&fb, 30.0);
    uint64_t after_stall = fb.stream.sent;
    assert(fb.stream.last_frame == 30.0);
    for (unsigned tick=1; tick<=7; ++tick) {
        ++fb.pixels[100]; framebuffer_stream(&fb, 30.0+tick*0.001);
    }
    assert(fb.stream.sent == after_stall);
    framebuffer_stream(&fb, 30.009); assert(fb.stream.sent == after_stall+1);
    fb.stream.last_frame = 8.05;
    fb.pixels[10]++;
    assert(framebuffer_capture(&fb, 9.0, 0) == 0 && fb.generation == 3);
    assert(framebuffer_capture(&fb, 9.0, 1) == 0 && fb.generation == 4);
    assert(close(sockets[1]) == 0);
    framebuffer_stream(&fb, 9.1);
    assert(!fb.stream.ring && fb.stream.socket == -1);
    fb.pixels[10]++;
    assert(framebuffer_capture(&fb, 9.3, 0) == 0 && fb.generation == 5);
    // A partially initialized framebuffer must not dereference NULL.
    struct target_framebuffer empty={.directory=fb.directory,.stream_initialized=1};
    assert(!framebuffer_capture(&empty,30,1));framebuffer_stream(&empty,30);
    assert(unlinkat(fb.directory, "frame-latest.bgra", 0) == 0);
    assert(framebuffer_release(&fb) == 0);
    assert(fb.pixels == MAP_FAILED && fb.directory == -1);
    assert(framebuffer_capture(&fb, 10.0, 1) == 0);
    assert(framebuffer_release(&fb) == 0);
    assert(munmap(shared, SHARED_SIZE) == 0);
    assert(rmdir(dir) == 0);
    puts("TARGET_FRAMEBUFFER_TEST_PASS host_only=1 locking_modeled=1");
}
