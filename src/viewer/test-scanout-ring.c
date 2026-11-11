#define _GNU_SOURCE
#include "scanout-client.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <sched.h>
static struct scanout_ring *ring;
static unsigned done;
static void *producer(void *unused) {
    (void)unused;uint64_t *pixels=malloc(SCANOUT_BYTES);assert(pixels);
    for(uint64_t seq=1;seq<=160;++seq) {
        for(size_t p=0;p<SCANOUT_BYTES/8;++p)pixels[p]=seq^(uint64_t)p;
        int i;while((i=scanout_claim(ring))<0)sched_yield();
        scanout_publish(ring,(unsigned)i,pixels,seq,seq*100);
    }
    free(pixels);__atomic_store_n(&done,1,__ATOMIC_RELEASE);return NULL;
}
static void descriptors(int wrong_size,int unsealed,int wrong_magic) {
    int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    int fd=memfd_create("test-scanout",MFD_ALLOW_SEALING);assert(fd>=0);
    assert(!ftruncate(fd,wrong_size?4096:SCANOUT_SIZE));
    uint32_t header[]={wrong_magic?0:SCANOUT_MAGIC,2,SCANOUT_BYTES,0};
    assert(pwrite(fd,header,sizeof(header),0)==sizeof(header));
    if(!unsealed)assert(!fcntl(fd,F_ADD_SEALS,F_SEAL_SEAL|F_SEAL_SHRINK|F_SEAL_GROW));
    char b=2;union {struct cmsghdr align;char data[CMSG_SPACE(sizeof(fd))];} control={0};
    struct iovec io={.iov_base=&b,.iov_len=1};struct msghdr m={.msg_iov=&io,.msg_iovlen=1,.msg_control=control.data,.msg_controllen=sizeof(control)};
    struct cmsghdr *h=CMSG_FIRSTHDR(&m);h->cmsg_level=SOL_SOCKET;h->cmsg_type=SCM_RIGHTS;h->cmsg_len=CMSG_LEN(sizeof(fd));memcpy(CMSG_DATA(h),&fd,sizeof(fd));
    assert(sendmsg(pair[0],&m,0)==1);close(fd);
    struct scanout_client c={.socket=pair[1],.previous=-1};
    int expected=!wrong_size&&!unsealed&&!wrong_magic;
    assert(scanout_receive(&c,0)==expected);
    if(expected){assert(c.ring);assert(scanout_receive(&c,1)==1);close(pair[0]);pair[0]=-1;assert(scanout_receive(&c,2)==0);}
    assert(c.ring==NULL && c.socket==-1);
    scanout_disconnect(&c);if(pair[0]>=0)close(pair[0]);
}
int main(void) {
    ring=calloc(1,SCANOUT_SIZE);assert(ring);
    for(unsigned i=0;i<3;++i)ring->slot[i].state=SCANOUT_READING;
    assert(scanout_claim(ring)==-1);scanout_release(ring,1);assert(scanout_claim(ring)==1);
    memset(ring,0,SCANOUT_HEADER);
    uint64_t seqs[]={9,3,7};
    for(unsigned i=0;i<3;++i){ring->slot[i].sequence=seqs[i];ring->slot[i].state=SCANOUT_READY;}
    assert(scanout_acquire(ring,0)==0);assert(ring->slot[1].state==SCANOUT_FREE && ring->slot[2].state==SCANOUT_FREE);
    scanout_release(ring,0);assert(scanout_acquire(ring,9)==-1);
    memset(ring,0,SCANOUT_HEADER);
    pthread_t thread;assert(!pthread_create(&thread,NULL,producer,NULL));
    uint64_t previous=0;unsigned read=0;
    while(!__atomic_load_n(&done,__ATOMIC_ACQUIRE) || previous<160) {
        int i=scanout_acquire(ring,previous);if(i<0){sched_yield();continue;}
        uint64_t seq=ring->slot[i].sequence;assert(seq>previous && ring->slot[i].captured_ns==seq*100);
        uint64_t *pixels=scanout_pixels(ring,(unsigned)i);
        for(size_t p=0;p<SCANOUT_BYTES/8;++p)assert(pixels[p]==(seq^(uint64_t)p));
        previous=seq;++read;scanout_release(ring,(unsigned)i);
    }
    assert(!pthread_join(thread,NULL));free(ring);
    descriptors(0,0,0);descriptors(1,0,0);descriptors(0,1,0);descriptors(0,0,1);
    printf("SCANOUT_RING_TEST_PASS full_frame_integrity=1 sampled=%u latest=160 invalid_fd_rejected=3\n",read);
    return 0;
}
