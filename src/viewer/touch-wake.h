/* Wake on atomic touch-file publication; retain bounded polling as fallback. */
#ifndef TOUCH_WAKE_H
#define TOUCH_WAKE_H
#include <sys/inotify.h>
#include <poll.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
static inline uint64_t tw_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static int tw_open(int first, int second) {
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return -1;
    int dirs[2] = {first, second};
    for (int i = 0; i < 2; ++i) {
        if (dirs[i] < 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", dirs[i]);
        if (inotify_add_watch(fd, path, IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF) < 0) {
            close(fd); return -1;
        }
    }
    return fd;
}
static void tw_wait(int fd, int timeout_ms) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, timeout_ms) > 0 && (p.revents & POLLIN)) {
        char events[8192];
        /* Notifications are hints. Callers re-read and validate the files. */
        (void)read(fd, events, sizeof(events));
    }
}
#endif
