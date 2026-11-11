/* Isolated transport qualification. No VM or guest memory is opened. */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

int main(int argc, char **argv) {
    if ((argc != 3 && argc != 4) || strlen(argv[2]) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return 2;
    int writable = argc == 4 && !strcmp(argv[3], "rw");
    if (argc == 4 && !writable) return 2;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) { perror("socket"); return 3; }
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    strcpy(a.sun_path, argv[2]);
    socklen_t address_size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(argv[2]) + 1);
    if (argv[2][0] == '@') { a.sun_path[0] = 0; --address_size; }
    struct timeval timeout = {.tv_sec = 12};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    int server = !strncmp(argv[1], "server", 6);
    int sender = !strcmp(argv[1], "server-send") || !strcmp(argv[1], "client");
    if (server) {
        if (bind(s, (void *)&a, address_size) || listen(s, 1)) { perror("bind/listen"); return 3; }
        puts("FD_PROBE_LISTENING"); fflush(stdout);
        int p = accept4(s, NULL, NULL, SOCK_CLOEXEC);
        if (p < 0) { perror("accept"); return 3; }
        close(s); s = p;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    } else if (connect(s, (void *)&a, address_size)) { perror("connect"); return 3; }
    char byte = 1, control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec io = {.iov_base = &byte, .iov_len = 1};
    struct msghdr m = {.msg_iov = &io, .msg_iovlen = 1,
        .msg_control = control, .msg_controllen = sizeof(control)};
    int fd;
    if (sender) {
        int rw = memfd_create("scanout-transport-probe", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        const char marker[] = "SCANOUT_FD_TRANSPORT_ONLY";
        if (rw < 0 || write(rw, marker, sizeof(marker)) != sizeof(marker)) { perror("create/write memfd"); return 4; }
        char path[64]; snprintf(path, sizeof(path), "/proc/self/fd/%d", rw);
        fd = writable ? dup(rw) : open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) { perror("open readonly memfd"); return 4; }
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(fd));
        memcpy(CMSG_DATA(c), &fd, sizeof(fd));
        if (sendmsg(s, &m, MSG_NOSIGNAL) != 1) { perror("sendmsg"); return 4; }
        close(rw);
    } else {
        ssize_t received = recvmsg(s, &m, MSG_CMSG_CLOEXEC);
        if (received != 1 || (m.msg_flags & MSG_CTRUNC)) { perror("recvmsg"); fprintf(stderr, "received=%zd flags=%x\n", received, m.msg_flags); return 4; }
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS || c->cmsg_len != CMSG_LEN(sizeof(fd))) { fputs("invalid control header\n", stderr); return 4; }
        memcpy(&fd, CMSG_DATA(c), sizeof(fd));
        char data[64] = {0};
        ssize_t bytes = pread(fd, data, sizeof(data), 0);
        int flags = fcntl(fd, F_GETFL);
        if (bytes < 1 || strcmp(data, "SCANOUT_FD_TRANSPORT_ONLY") || (flags & O_ACCMODE) != (writable ? O_RDWR : O_RDONLY)) {
            perror("read descriptor"); fprintf(stderr, "bytes=%zd flags=%x data=%s\n", bytes, flags, data); return 4;
        }
        if (writable && pwrite(fd, "OK", 2, 0) != 2) { perror("write descriptor"); return 4; }
        printf("FD_PROBE_PASS readonly=%d no_guest_memory=1\n", !writable);
        if (argv[2][0] != '@') unlink(argv[2]);
    }
    close(fd); close(s); return 0;
}
