/* Read-only relay for atomically published shared scanout. No VM descriptors,
 * private RAM access, commands, or protected-partition writes. Loopback only. */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#define FRAME_BYTES (1536u*2048u*4u)
static int transfer(int fd, void *buf, size_t len, int send_data) {
    unsigned char *p=buf;
    while(len) {
        ssize_t n=send_data ? send(fd,p,len,MSG_NOSIGNAL) : read(fd,p,len);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) return -1;
        p+=n; len-=(size_t)n;
    }
    return 0;
}
static void be32(unsigned char *p, uint32_t n) { n=htonl(n); memcpy(p,&n,4); }
static void be64(unsigned char *p, uint64_t n) { for(int i=7;i>=0;--i) {p[i]=(unsigned char)n;n>>=8;} }
static uint64_t read64(const unsigned char *p) { uint64_t n=0;for(int i=0;i<8;++i)n=(n<<8)|p[i];return n; }
int main(int argc,char **argv) {
    if(argc!=4 || strlen(argv[2])!=64) return 2;
    for(int i=0;i<64;++i) if(!strchr("0123456789abcdef",argv[2][i])) return 2;
    char *end; long port=strtol(argv[3],&end,10);
    if(*end || port<1024 || port>65535) return 2;
    int dir=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0) {perror("directory");return 3;}
    int server=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons((uint16_t)port),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    if(server<0 || bind(server,(void*)&addr,sizeof(addr)) || listen(server,2)) {perror("listen");return 3;}
    unsigned char *pixels=malloc(FRAME_BYTES);
    if(!pixels) return 3;
    signal(SIGPIPE,SIG_IGN);
    printf("SCANOUT_RELAY_READY port=%ld\n",port);fflush(stdout);
    for(;;) {
        int peer=accept4(server,NULL,NULL,SOCK_CLOEXEC);
        if(peer<0) {if(errno==EINTR)continue;perror("accept");return 4;}
        struct timeval timeout={.tv_sec=3};
        setsockopt(peer,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        setsockopt(peer,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        unsigned char req[72], header[24]; int fd=-1;
        if(transfer(peer,req,sizeof(req),0)) goto done;
        unsigned diff=0;for(int i=0;i<64;++i)diff|=req[i]^(unsigned char)argv[2][i];
        if(diff) goto done;
        fd=openat(dir,"frame-latest.bgra",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        struct stat st;
        if(fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size!=FRAME_BYTES) goto done;
        uint64_t generation=((uint64_t)st.st_mtim.tv_sec<<32) ^ (uint32_t)st.st_mtim.tv_nsec ^ (uint64_t)st.st_ino;
        int changed=generation!=read64(req+64);
        if(changed) {
            size_t pos=0;
            while(pos<FRAME_BYTES) {
                ssize_t n=read(fd,pixels+pos,FRAME_BYTES-pos);
                if(n<0 && errno==EINTR)continue;
                if(n<=0)goto done;
                pos+=(size_t)n;
            }
        }
        be32(header,0x46524d31);be32(header+4,1536);be32(header+8,2048);
        be32(header+12,changed?FRAME_BYTES:0);be64(header+16,generation);
        if(transfer(peer,header,sizeof(header),1))goto done;
        if(changed)transfer(peer,pixels,FRAME_BYTES,1);
done:
        if(fd>=0)close(fd);
        close(peer);
    }
}
