/* Snapshot latched until guest acknowledgment. No access to private RAM. */
#ifndef TARGET_TOUCH_H
#define TARGET_TOUCH_H
#include "../guest/GunyahTouch.h"
static struct gt_frame gt_active;
static uint32_t gt_completed;
static uint32_t gt_pending_ack;
static unsigned gt_ack_attempts, gt_rejected;
static uint64_t gt_ack_serial;
static double gt_ack_retry_at;
static int gt_recent(struct timespec timestamp, struct timespec now) {
    if (timestamp.tv_sec > now.tv_sec || timestamp.tv_sec < now.tv_sec - 2) return 0;
    int64_t age = (now.tv_sec - timestamp.tv_sec) * INT64_C(1000000000) +
        now.tv_nsec - timestamp.tv_nsec;
    return age >= 0 && age <= INT64_C(2000000000);
}

/* A consumed HID frame is never replayed when publishing its ACK fails.
 * Retry on later polls, keeping the immutable snapshot until the relay can
 * observe completion. Touch transport failures do not stop the VM. */
static int gt_publish_ack(void) {
    if (!gt_pending_ack) return 0;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    double seconds = now.tv_sec + now.tv_nsec / 1e9;
    if (seconds < gt_ack_retry_at) return -1;
    gt_ack_retry_at = seconds + 0.008;
    char temporary[96];
    snprintf(temporary, sizeof(temporary), ".touch-ack-%ld-%u-%" PRIu64 ".tmp",
             (long)getpid(), gt_pending_ack, ++gt_ack_serial);
    int fd = openat(gd_directory, temporary,
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    int error = fd < 0 ? errno : 0;
    if (fd >= 0) {
        if (gd_transfer(fd, &gt_pending_ack, 4, 1)) error = errno ? errno : EIO;
        if (close(fd) && !error) error = errno;
        if (!error && renameat(gd_directory, temporary, gd_directory, "touch.ack")) error = errno;
        if (error) unlinkat(gd_directory, temporary, 0);
    }
    if (error) {
        ++gt_ack_attempts;
        if (gt_ack_attempts == 1 || !(gt_ack_attempts % 64))
            printf("TOUCH_ACK_RETRY sequence=%u attempts=%u errno=%d\n",
                   gt_pending_ack, gt_ack_attempts, error);
        return -1;
    }
    if (gt_ack_attempts)
        printf("TOUCH_ACK_RECOVERED sequence=%u retries=%u\n", gt_pending_ack, gt_ack_attempts);
    gt_completed = gt_pending_ack;
    gt_pending_ack = 0;
    gt_ack_attempts = 0;
    gt_ack_retry_at = 0;
    memset(&gt_active, 0, sizeof(gt_active));
    return 0;
}
static uint32_t gt_poll(void) {
    if (gt_pending_ack && gt_publish_ack()) return 0;
    if (gt_active.sequence) return gt_active.sequence;
    if (gd_directory < 0) return 0;
    int fd = openat(gd_directory, "touch.request", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return 0;
    struct stat st;
    struct gt_frame frame;
    struct timespec now;
    int valid = !clock_gettime(CLOCK_REALTIME, &now) && !fstat(fd, &st) &&
        S_ISREG(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 0077) &&
        st.st_nlink == 1 && st.st_size == sizeof(frame) &&
        !gd_transfer(fd, &frame, sizeof(frame), 0) && gt_valid(&frame);
    close(fd);
    /* The relay leaves the completed request in place; idle polls are normal. */
    if (valid && frame.sequence <= gt_completed) return 0;
    if (valid) valid = gt_recent(st.st_mtim, now);
    if (!valid) {
        if (++gt_rejected == 1 || !(gt_rejected % 1024))
            printf("TOUCH_REQUEST_REJECTED count=%u completed=%u\n", gt_rejected, gt_completed);
        return 0;
    }
    gt_active = frame;
    return frame.sequence;
}
static int gt_mmio(uint64_t address, unsigned length, unsigned char *data, int writing) {
    if (length != 4 || address % 4) return -1;
    uint32_t value = 0;
    if (address == GT_POLL && !writing) { value = gt_poll(); memcpy(data, &value, 4); return 0; }
    if (address >= GT_DATA && address < GT_DATA + sizeof(gt_active) && !writing) {
        memcpy(data, (unsigned char *)&gt_active + (address - GT_DATA), 4); return 0;
    }
    if (address == GT_ACK && writing) {
        memcpy(&value, data, 4);
        if (!value) return -1;
        if (value == gt_completed || value == gt_pending_ack) return 0;
        if (value != gt_active.sequence) return -1;
        gt_pending_ack = value;
        (void)gt_publish_ack();
        return 0;
    }
    return -1;
}
#endif
