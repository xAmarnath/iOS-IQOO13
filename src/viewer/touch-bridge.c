/* Root copies only validated touch snapshots/acks; never signals the VM owner. */
#define _GNU_SOURCE
#include "touch-files.h"
#include "touch-wake.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
int main(int argc,char **argv) {
    if(argc!=4)return 2;
    char *end;long pid=strtol(argv[3],&end,10);
    if(*end || pid<=1 || pid>INT_MAX)return 2;
    char proc[64],expected[PATH_MAX],actual[PATH_MAX];
    snprintf(proc,sizeof(proc),"/proc/%ld/exe",pid);
    if(snprintf(expected,sizeof(expected),"%s/monitor",argv[1])>=(int)sizeof(expected))return 2;
    int guest=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int viewer=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(guest<0 || viewer<0)return 3;
    int wake=tw_open(guest,viewer);
    uint32_t sent=0,returned=0;
    if(!tf_read(guest,"touch.ack",&returned,4,0)) {
        sent=returned;
        if(tf_write(viewer,"touch.ack",&returned,4,1))return 4;
    }
    printf("TOUCH_BRIDGE_READY owner=%ld\n",pid);fflush(stdout);
    for(;;) {
        ssize_t n=readlink(proc,actual,sizeof(actual)-1);
        if(n<0)break;
        actual[n]=0;
        if(strcmp(actual,expected))break;
        struct gt_frame frame;
        if(!tf_read(viewer,"touch.request",&frame,sizeof(frame),1) && gt_valid(&frame) && frame.sequence>sent) {
            if(tf_write(guest,"touch.request",&frame,sizeof(frame),0))return 4;
            sent=frame.sequence;
        }
        uint32_t ack=0;
        if(!tf_read(guest,"touch.ack",&ack,4,0) && ack>returned && ack<=sent) {
            if(tf_write(viewer,"touch.ack",&ack,4,1))return 4;
            returned=ack;
        }
        tw_wait(wake,10);
    }
    puts("TOUCH_BRIDGE_OWNER_ENDED");return 0;
}
