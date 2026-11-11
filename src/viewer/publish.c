/* Copies only complete, public scanout frames to the shell relay directory.
 * It never opens a VM descriptor, private guest RAM or a block device. */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#define FRAME_BYTES (1536u*2048u*4u)
#define POLL_US 50000u
static int same_frame(const struct stat *a,const struct stat *b) {
    return a->st_ino==b->st_ino && a->st_mtim.tv_sec==b->st_mtim.tv_sec &&
        a->st_mtim.tv_nsec==b->st_mtim.tv_nsec;
}
int main(int argc,char **argv) {
    if(argc!=4) return 2;
    char *end;long pid=strtol(argv[3],&end,10);
    if(*end || pid<=1 || pid>INT_MAX) return 2;
    char proc[64],expected[PATH_MAX],actual[PATH_MAX],temp[64];
    snprintf(proc,sizeof(proc),"/proc/%ld/exe",pid);
    if(snprintf(expected,sizeof(expected),"%s/monitor",argv[1])>=(int)sizeof(expected))return 2;
    snprintf(temp,sizeof(temp),".scanout-%ld.tmp",(long)getpid());
    int src=open(argv[1],O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int dst=open(argv[2],O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(src<0 || dst<0)return 3;
    unsigned char *buf=malloc(FRAME_BYTES);
    if(!buf)return 3;
    struct stat previous={0};
    printf("SCANOUT_PUBLISHER_READY owner=%ld\n",pid);fflush(stdout);
    for(;;) {
        ssize_t n=readlink(proc,actual,sizeof(actual)-1);
        if(n<0)break;
        actual[n]=0;if(strcmp(actual,expected))break;
        int in=openat(src,"frame-latest.bgra",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        struct stat st;
        if(in<0 || fstat(in,&st) || !S_ISREG(st.st_mode) || st.st_size!=FRAME_BYTES) {
            if(in>=0)close(in);
            usleep(POLL_US);continue;
        }
        if(same_frame(&previous,&st)){close(in);usleep(POLL_US);continue;}
        size_t pos=0;
        while(pos<FRAME_BYTES) {
            n=read(in,buf+pos,FRAME_BYTES-pos);
            if(n<0 && errno==EINTR)continue;
            if(n<=0)break;
            pos+=(size_t)n;
        }
        close(in);
        if(pos!=FRAME_BYTES){usleep(POLL_US);continue;}
        int out=openat(dst,temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(out<0){perror("create frame");return 4;}
        pos=0;
        while(pos<FRAME_BYTES) {
            n=write(out,buf+pos,FRAME_BYTES-pos);
            if(n<0 && errno==EINTR)continue;
            if(n<=0)break;
            pos+=(size_t)n;
        }
        int ok=pos==FRAME_BYTES && !fchown(out,2000,2000);
        if(close(out))ok=0;
        if(ok && !renameat(dst,temp,dst,"frame-latest.bgra")) {
            previous=st;
            printf("SCANOUT_PUBLISHED inode=%llu\n",(unsigned long long)st.st_ino);fflush(stdout);
        } else {
            unlinkat(dst,temp,0);perror("publish frame");return 4;
        }
        usleep(POLL_US);
    }
    puts("SCANOUT_PUBLISHER_OWNER_ENDED");
    return 0;
}
