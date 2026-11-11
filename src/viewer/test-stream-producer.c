/* Synthetic transport benchmark. This is NOT guest/iPadOS rendering. */
#define _GNU_SOURCE
#include "scanout-client.h"
#include <stdlib.h>
#include <time.h>
static double seconds(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9; }
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    char *end;unsigned long uid=strtoul(argv[2],&end,10);if(*end||uid<10000||uid>1000000)return 2;
    struct scanout_client c={.socket=-1,.previous=-1};
    double start=seconds();
    if(scanout_connect(&c,argv[1],(unsigned)uid,start)){perror("connect");return 3;}
    while(!scanout_receive(&c,seconds()) && seconds()-start<3)usleep(1000);
    if(!c.ring){puts("SYNTHETIC_STREAM_NO_BUFFER");return 4;}
    unsigned char *pixels=calloc(1,SCANOUT_BYTES);if(!pixels)return 5;
    start=seconds();double copy_seconds=0;unsigned n=0;
    while(seconds()-start<20) {
        double now=seconds();
        if(!scanout_receive(&c,now))break;
        /* Blue / green / red thirds, moving white stripe, varying corner. */
        for(unsigned y=0;y<SCANOUT_HEIGHT;++y)for(unsigned x=0;x<SCANOUT_WIDTH;++x) {
            unsigned pos=(y*SCANOUT_WIDTH+x)*4;
            pixels[pos]=x<512?255:0;pixels[pos+1]=x>=512&&x<1024?255:0;pixels[pos+2]=x>=1024?255:0;
            if(x/16==n%96)pixels[pos]=pixels[pos+1]=pixels[pos+2]=255;
            pixels[pos+3]=255;
        }
        double begin=seconds();scanout_send(&c,pixels,begin);copy_seconds+=seconds()-begin;++n;
        double delay=(1.0/120.0)-(seconds()-now);
        if(delay>0)usleep((useconds_t)(delay*1e6));
    }
    printf("SYNTHETIC_STREAM_DONE seconds=%.3f iterations=%u published=%llu dropped=%llu copy_ms_total=%.3f guest_frames=0\n",seconds()-start,n,(unsigned long long)c.sent,(unsigned long long)c.dropped,copy_seconds*1e3);
    free(pixels);scanout_disconnect(&c);return 0;
}
