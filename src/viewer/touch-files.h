/* Bounded, atomic file messages shared by the unprivileged relay and bridge. */
#ifndef VIEWER_TOUCH_FILES_H
#define VIEWER_TOUCH_FILES_H
#include "../guest/GunyahTouch.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
static int tf_read(int dir, const char *name, void *data, size_t size, int fresh) {
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0)return -1;
    struct stat st; struct timespec now;
    clock_gettime(CLOCK_REALTIME,&now);
    int ok=!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size==(off_t)size;
    if(ok && fresh) {
        int64_t age=(int64_t)(now.tv_sec-st.st_mtim.tv_sec)*1000000000 + now.tv_nsec-st.st_mtim.tv_nsec;
        ok=age>=0 && age<=2000000000;
    }
    size_t pos=0;
    while(ok && pos<size) {
        ssize_t n=read(fd,(char*)data+pos,size-pos);
        if(n<0 && errno==EINTR)continue;
        if(n<=0){ok=0;break;}pos+=(size_t)n;
    }
    close(fd);return ok?0:-1;
}
static int tf_write(int dir,const char *name,const void *data,size_t size,int shell_owner) {
    char temp[80];snprintf(temp,sizeof(temp),".touch-%ld.tmp",(long)getpid());
    int fd=openat(dir,temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)return -1;
    size_t pos=0;
    while(pos<size) {
        ssize_t n=write(fd,(const char*)data+pos,size-pos);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)break;
        pos+=(size_t)n;
    }
    int ok=pos==size && (!shell_owner || !fchown(fd,2000,2000));
    if(close(fd))ok=0;
    if(ok && !renameat(dir,temp,dir,name))return 0;
    unlinkat(dir,temp,0);return -1;
}
#endif
