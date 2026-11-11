/* Authenticated loopback touch stream. One acknowledged full snapshot at a time. */
#define _GNU_SOURCE
#include "touch-files.h"
#include "touch-wake.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
static int transfer(int fd,void *buf,size_t len,int sending) {
    unsigned char *p=buf;
    while(len){ssize_t n=sending?send(fd,p,len,MSG_NOSIGNAL):read(fd,p,len);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)return -1;
        p+=n;len-=(size_t)n;}
    return 0;
}
static int submit(int dir,int wake,struct gt_frame *frame,uint32_t *sequence) {
    if(*sequence==UINT32_MAX)return -1;
    frame->sequence=++*sequence;
    if(tf_write(dir,"touch.request",frame,sizeof(*frame),0))return -1;
    uint64_t deadline=tw_now_ms()+2000;
    while(tw_now_ms()<deadline) {
        uint32_t ack=0;
        if(!tf_read(dir,"touch.ack",&ack,4,0) && ack==*sequence)return 0;
        tw_wait(wake,10);
    }
    fprintf(stderr,"TOUCH_ACK_TIMEOUT sequence=%u\n",*sequence);return -1;
}
int main(int argc,char **argv) {
    if(argc!=4 || strlen(argv[2])!=64)return 2;
    for(int i=0;i<64;++i)if(!strchr("0123456789abcdef",argv[2][i]))return 2;
    char *end;long port=strtol(argv[3],&end,10);
    if(*end || port<1024 || port>65535)return 2;
    int dir=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0)return 3;
    int wake=tw_open(dir,-1);
    uint32_t seq=0,ack=0;struct gt_frame old;
    if(!tf_read(dir,"touch.ack",&ack,4,0))seq=ack;
    if(!tf_read(dir,"touch.request",&old,sizeof(old),0) && gt_valid(&old) && old.sequence>seq)seq=old.sequence;
    int server=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0),one=1;
    setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons((uint16_t)port),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    if(server<0 || bind(server,(void*)&addr,sizeof(addr)) || listen(server,2))return 3;
    signal(SIGPIPE,SIG_IGN);
    printf("TOUCH_RELAY_READY port=%ld\n",port);fflush(stdout);
    for(;;) {
        int peer=accept4(server,NULL,NULL,SOCK_CLOEXEC);
        if(peer<0){if(errno==EINTR)continue;return 4;}
        struct timeval timeout={.tv_sec=2};
        setsockopt(peer,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        setsockopt(peer,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        char token[64];unsigned diff=0;int down=0,authenticated=0;
        if(transfer(peer,token,sizeof(token),0))goto done;
        for(unsigned i=0;i<64;++i)diff|=(unsigned char)token[i]^(unsigned char)argv[2][i];
        if(diff)goto done;
        authenticated=1;
        for(;;) {
            struct gt_frame frame;
            if(transfer(peer,&frame,sizeof(frame),0) || !gt_valid(&frame))break;
            down=frame.count!=0;
            if(submit(dir,wake,&frame,&seq))break;
            uint32_t accepted=1;
            if(transfer(peer,&accepted,4,1))break;
        }
done:
        close(peer);
        if(authenticated && down) {
            struct gt_frame release={.magic=GT_MAGIC,.version=GT_VERSION,.sequence=1};
            (void)submit(dir,wake,&release,&seq);
        }
    }
}
