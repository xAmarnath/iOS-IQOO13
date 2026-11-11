#define _GNU_SOURCE
#include "scanout-ring.h"
#include <jni.h>
#include <android/log.h>
#include <GLES2/gl2.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdlib.h>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO,"GuestStream",__VA_ARGS__)
struct viewer {
    int fd, server, peer, width, height;
    struct scanout_ring *ring;
    GLuint texture[3], program;
    unsigned current_texture;
    uint64_t sequence, uploads, started, last_report, upload_ns, age_ns, max_age_ns;
    int have_frame;
};
static uint64_t ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static void destroy(struct viewer *v) {
    if(v->peer>=0)close(v->peer);
    if(v->server>=0)close(v->server);
    if(v->ring)munmap(v->ring,SCANOUT_SIZE);
    if(v->fd>=0)close(v->fd);
    free(v);
}
JNIEXPORT jlong JNICALL Java_org_openoos_vmviewer_StreamActivity_nativeCreate(JNIEnv *env,jclass klass,jstring endpoint) {
    (void)klass;
    const char *name=(*env)->GetStringUTFChars(env,endpoint,NULL);
    if(!name)return 0;
    size_t len=strlen(name);
    struct viewer *v=calloc(1,sizeof(*v));
    if(!v){(*env)->ReleaseStringUTFChars(env,endpoint,name);return 0;}
    v->fd=v->server=v->peer=-1;
    struct sockaddr_un a={.sun_family=AF_UNIX};
    if(len<16 || len>=sizeof(a.sun_path)-1)goto fail;
    memcpy(a.sun_path+1,name,len);
    v->fd=memfd_create("guest-published-scanout",MFD_CLOEXEC|MFD_ALLOW_SEALING);
    if(v->fd<0 || ftruncate(v->fd,SCANOUT_SIZE) ||
        fcntl(v->fd,F_ADD_SEALS,F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL))goto fail;
    void *p=mmap(NULL,SCANOUT_SIZE,PROT_READ|PROT_WRITE,MAP_SHARED,v->fd,0);
    if(p==MAP_FAILED)goto fail;
    v->ring=p;
    v->ring->magic=SCANOUT_MAGIC; v->ring->version=2; v->ring->bytes=SCANOUT_BYTES;
    v->server=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(v->server<0 || bind(v->server,(void*)&a,(socklen_t)(offsetof(struct sockaddr_un,sun_path)+1+len)) || listen(v->server,1))goto fail;
    (*env)->ReleaseStringUTFChars(env,endpoint,name);
    LOG("READY transport=shared-memfd slots=3 bytes=%u",SCANOUT_SIZE);
    return (jlong)(uintptr_t)v;
fail:
    LOG("CREATE_FAILED errno=%d",errno);
    (*env)->ReleaseStringUTFChars(env,endpoint,name); destroy(v); return 0;
}
static void serve(struct viewer *v) {
    if(v->peer>=0) {
        char b; ssize_t n=recv(v->peer,&b,1,MSG_PEEK|MSG_DONTWAIT);
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))return;
        close(v->peer); v->peer=-1;
    }
    int s=accept4(v->server,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);
    if(s<0)return;
    struct ucred peer; socklen_t size=sizeof(peer);
    if(getsockopt(s,SOL_SOCKET,SO_PEERCRED,&peer,&size) || peer.uid!=0){close(s);return;}
    char b=2; union { struct cmsghdr align; char data[CMSG_SPACE(sizeof(int))]; } control={0};
    struct iovec io={.iov_base=&b,.iov_len=1};
    struct msghdr m={.msg_iov=&io,.msg_iovlen=1,.msg_control=control.data,.msg_controllen=sizeof(control)};
    struct cmsghdr *h=CMSG_FIRSTHDR(&m);h->cmsg_level=SOL_SOCKET;h->cmsg_type=SCM_RIGHTS;h->cmsg_len=CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(h),&v->fd,sizeof(int));
    if(sendmsg(s,&m,MSG_NOSIGNAL)!=1){LOG("FD_SEND_FAILED errno=%d",errno);close(s);return;}
    v->peer=s;v->sequence=0;v->started=v->last_report=ns();v->uploads=v->age_ns=v->max_age_ns=v->upload_ns=0;
    LOG("PRODUCER_CONNECTED uid=0");
}
static GLuint shader(GLenum type,const char *text) {
    GLuint s=glCreateShader(type);glShaderSource(s,1,&text,NULL);glCompileShader(s);
    GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){char msg[512];glGetShaderInfoLog(s,sizeof(msg),NULL,msg);LOG("SHADER_FAILED %s",msg);glDeleteShader(s);return 0;}
    return s;
}
JNIEXPORT void JNICALL Java_org_openoos_vmviewer_StreamActivity_nativeSurface(JNIEnv *env,jclass klass,jlong handle) {
    (void)env;(void)klass;struct viewer *v=(void*)(uintptr_t)handle;
    GLuint vs=shader(GL_VERTEX_SHADER,"attribute vec2 p;varying vec2 uv;void main(){gl_Position=vec4(p,0.,1.);uv=vec2((p.x+1.)*.5,(1.-p.y)*.5);}");
    GLuint fs=shader(GL_FRAGMENT_SHADER,"precision mediump float;varying vec2 uv;uniform sampler2D tex;void main(){vec4 c=texture2D(tex,uv);gl_FragColor=vec4(c.bgr,1.);}");
    v->program=glCreateProgram();glAttachShader(v->program,vs);glAttachShader(v->program,fs);glBindAttribLocation(v->program,0,"p");glLinkProgram(v->program);
    glDeleteShader(vs);glDeleteShader(fs);
    GLint ok;glGetProgramiv(v->program,GL_LINK_STATUS,&ok);if(!ok){LOG("LINK_FAILED");return;}
    glGenTextures(3,v->texture);v->current_texture=0;
    for(unsigned i=0;i<3;++i) {
        glBindTexture(GL_TEXTURE_2D,v->texture[i]);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,SCANOUT_WIDTH,SCANOUT_HEIGHT,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    }
    v->have_frame=0;
    /* Context recreation needs a fresh upload even when guest pixels are unchanged. */
    __atomic_store_n(&v->ring->reserved, 1, __ATOMIC_RELEASE);
    LOG("SURFACE renderer=%s",glGetString(GL_RENDERER));
}
JNIEXPORT void JNICALL Java_org_openoos_vmviewer_StreamActivity_nativeSize(JNIEnv *env,jclass klass,jlong handle,jint w,jint h) {
    (void)env;(void)klass;struct viewer *v=(void*)(uintptr_t)handle;v->width=w;v->height=h;
}
JNIEXPORT void JNICALL Java_org_openoos_vmviewer_StreamActivity_nativeDraw(JNIEnv *env,jclass klass,jlong handle) {
    (void)env;(void)klass;struct viewer *v=(void*)(uintptr_t)handle;
    serve(v);
    int i=scanout_acquire(v->ring,v->sequence);
    if(i>=0) {
        uint64_t begin=ns(),captured=v->ring->slot[i].captured_ns;
        unsigned next=(v->current_texture+1)%3;
        glBindTexture(GL_TEXTURE_2D,v->texture[next]);
        glTexSubImage2D(GL_TEXTURE_2D,0,0,0,SCANOUT_WIDTH,SCANOUT_HEIGHT,GL_RGBA,GL_UNSIGNED_BYTE,scanout_pixels(v->ring,(unsigned)i));
        GLenum error=glGetError();
        v->sequence=v->ring->slot[i].sequence;
        /* OpenGL copies client memory before return; producer can reuse this slot. */
        scanout_release(v->ring,(unsigned)i);
        if(error){LOG("UPLOAD_FAILED error=%u",error);__atomic_store_n(&v->ring->reserved,1,__ATOMIC_RELEASE);return;}
        v->current_texture=next;
        v->have_frame=1;v->upload_ns+=ns()-begin;
        uint64_t age=begin>=captured?begin-captured:0;
        v->age_ns+=age;if(age>v->max_age_ns)v->max_age_ns=age;++v->uploads;
    }
    glViewport(0,0,v->width,v->height);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    if(v->have_frame) {
        float scale=(float)v->width/SCANOUT_WIDTH;
        if((float)v->height/SCANOUT_HEIGHT<scale)scale=(float)v->height/SCANOUT_HEIGHT;
        int w=(int)(SCANOUT_WIDTH*scale),h=(int)(SCANOUT_HEIGHT*scale);
        glViewport((v->width-w)/2,(v->height-h)/2,w,h);glUseProgram(v->program);
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,v->texture[v->current_texture]);
        glUniform1i(glGetUniformLocation(v->program,"tex"),0);
        const GLfloat vertices[]={-1,-1,1,-1,-1,1,1,1};
        glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,vertices);glEnableVertexAttribArray(0);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    }
    uint64_t now=ns();
    if(v->peer>=0 && now-v->last_report>=UINT64_C(5000000000)) {
        double seconds=(now-v->last_report)/1e9;
        LOG("METRICS seconds=%.3f uploaded=%llu fps=%.2f copy_to_upload_avg_ms=%.3f max_ms=%.3f upload_avg_ms=%.3f sequence=%llu",
            seconds,(unsigned long long)v->uploads,v->uploads/seconds,
            v->uploads?v->age_ns/(1e6*v->uploads):0,v->max_age_ns/1e6,
            v->uploads?v->upload_ns/(1e6*v->uploads):0,(unsigned long long)v->sequence);
        v->last_report=now;v->uploads=v->age_ns=v->max_age_ns=v->upload_ns=0;
    }
}
JNIEXPORT void JNICALL Java_org_openoos_vmviewer_StreamActivity_nativeClose(JNIEnv *env,jclass klass,jlong handle) {
    (void)env;(void)klass;destroy((void*)(uintptr_t)handle);
}
