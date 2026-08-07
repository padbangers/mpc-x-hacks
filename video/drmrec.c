/* drmrec - stream the MPC X screen as a sequence of PNG frames to stdout.
 * DRM opened once; per frame: bulk-memcpy the (uncached) scanout fb into cached
 * RAM, build portrait RGB scanlines, deflate (PNG). NO rotation here (too slow on
 * the uncached path / CPU) -> the receiving PC rotates + encodes with ffmpeg.
 * Frames are concatenated PNGs (ffmpeg image2pipe).
 * Usage: drmrec [seconds 0=inf] [fps 12] [zlevel 1] [/dev/dri/card0]   (SIGINT/TERM = stop)
 */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

typedef struct _drmModePlaneRes { uint32_t count_planes; uint32_t*planes; } drmModePlaneRes, *drmModePlaneResPtr;
typedef struct _drmModePlane { uint32_t count_formats; uint32_t*formats; uint32_t plane_id, crtc_id, fb_id,
    crtc_x, crtc_y, x, y, possible_crtcs, gamma_size; } drmModePlane, *drmModePlanePtr;
typedef struct _drmModeFB2 { uint32_t fb_id, width, height, pixel_format; uint64_t modifier;
    uint32_t flags; uint32_t handles[4]; uint32_t pitches[4]; uint32_t offsets[4]; } drmModeFB2, *drmModeFB2Ptr;
extern drmModePlaneResPtr drmModeGetPlaneResources(int fd);
extern drmModePlanePtr drmModeGetPlane(int fd, uint32_t plane_id);
extern drmModeFB2Ptr drmModeGetFB2(int fd, uint32_t fb_id);
extern int drmSetClientCap(int fd, uint64_t cap, uint64_t val);
extern int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
extern void drmModeFreePlane(void*); extern void drmModeFreeFB2(void*); extern void drmModeFreePlaneResources(void*);
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
struct dma_buf_sync { uint64_t flags; };
#define DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct dma_buf_sync)
extern int deflateInit2_(void*,int,int,int,int,int,const char*,int);
extern int deflate(void*,int); extern int deflateEnd(void*); extern unsigned long deflateBound(void*,unsigned long);
extern unsigned long crc32(unsigned long,const unsigned char*,unsigned int);
typedef struct z_stream_s { unsigned char*ni; unsigned int ai; unsigned long ti; unsigned char*no; unsigned int ao;
    unsigned long to; char*msg; void*st; void*za; void*zf; void*op; int dt; unsigned long ad; unsigned long rs; } z_stream;

static volatile int stop=0;
static void onsig(int s){ (void)s; stop=1; }
static void put_be32(unsigned char*b,uint32_t v){ b[0]=v>>24;b[1]=v>>16;b[2]=v>>8;b[3]=v; }
static void chunk(FILE*f,const char*ty,const unsigned char*d,uint32_t n){
    unsigned char h[4]; put_be32(h,n); fwrite(h,1,4,f); fwrite(ty,1,4,f); if(n) fwrite(d,1,n,f);
    unsigned long c=crc32(0,(const unsigned char*)ty,4); if(n)c=crc32(c,d,n);
    unsigned char cc[4]; put_be32(cc,(uint32_t)c); fwrite(cc,1,4,f);
}
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static uint32_t find_fb(int fd){ drmModePlaneResPtr pr=drmModeGetPlaneResources(fd); uint32_t best=0; uint64_t area=0;
    for(uint32_t i=0;pr&&i<pr->count_planes;i++){ drmModePlanePtr pl=drmModeGetPlane(fd,pr->planes[i]);
        if(pl&&pl->fb_id){ drmModeFB2Ptr fb=drmModeGetFB2(fd,pl->fb_id); if(fb){ uint64_t a=(uint64_t)fb->width*fb->height;
            if(a>area){area=a;best=pl->fb_id;} drmModeFreeFB2(fb);} } if(pl)drmModeFreePlane(pl);} if(pr)drmModeFreePlaneResources(pr); return best; }

int main(int argc,char**argv){
    double secs=argc>1?atof(argv[1]):0; int fps=argc>2?atoi(argv[2]):12;
    int zlevel=argc>3?atoi(argv[3]):1; const char*dev=argc>4?argv[4]:"/dev/dri/card0";
    if(fps<1)fps=1; double period=1.0/fps;
    signal(SIGINT,onsig); signal(SIGTERM,onsig);
    int fd=open(dev,O_RDWR); if(fd<0){perror("open");return 1;}
    drmSetClientCap(fd,DRM_CLIENT_CAP_UNIVERSAL_PLANES,1);
    unsigned char*loc=NULL,*raw=NULL,*comp=NULL; size_t loccap=0,rawcap=0,compcap=0;
    static const unsigned char sig[8]={0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};
    double t_start=now(), next=t_start; long frames=0;
    while(!stop){
        double tnow=now();
        if(secs>0 && (tnow-t_start)>=secs) break;
        if(tnow<next){ struct timespec ts={0,(long)((next-tnow)*1e9)}; nanosleep(&ts,NULL); }
        next+=period;
        uint32_t fbid=find_fb(fd); if(!fbid) continue;
        drmModeFB2Ptr fb=drmModeGetFB2(fd,fbid); uint32_t W=fb->width,H=fb->height,pitch=fb->pitches[0];
        int pfd=-1; if(drmPrimeHandleToFD(fd,fb->handles[0],O_RDONLY,&pfd)){drmModeFreeFB2(fb);continue;}
        size_t sz=(size_t)pitch*H;
        unsigned char*map=mmap(NULL,sz,PROT_READ,MAP_SHARED,pfd,0);
        if(map==MAP_FAILED){close(pfd);drmModeFreeFB2(fb);continue;}
        struct dma_buf_sync s={1}; ioctl(pfd,DMA_BUF_IOCTL_SYNC,&s);
        if(sz>loccap){loc=realloc(loc,sz);loccap=sz;} memcpy(loc,map,sz);
        munmap(map,sz); close(pfd);
        size_t rl=(size_t)H*(1+(size_t)W*3); if(rl>rawcap){raw=realloc(raw,rl);rawcap=rl;}
        size_t q=0; for(uint32_t y=0;y<H;y++){ raw[q++]=0; unsigned char*rw=loc+(size_t)y*pitch;
            for(uint32_t x=0;x<W;x++){ unsigned char*p=rw+(size_t)x*4; raw[q++]=p[2];raw[q++]=p[1];raw[q++]=p[0]; } }
        drmModeFreeFB2(fb);
        z_stream zs; memset(&zs,0,sizeof zs); deflateInit2_(&zs,zlevel,8,15,8,0,"1.3.1",(int)sizeof(z_stream));
        unsigned long bound=deflateBound(&zs,rl); if(bound>compcap){comp=realloc(comp,bound);compcap=bound;}
        zs.ni=raw;zs.ai=(unsigned)rl;zs.no=comp;zs.ao=(unsigned)bound; deflate(&zs,4); unsigned long cl=zs.to; deflateEnd(&zs);
        /* emit one PNG */
        fwrite(sig,1,8,stdout);
        unsigned char ih[13]; put_be32(ih,W); put_be32(ih+4,H); ih[8]=8;ih[9]=2;ih[10]=0;ih[11]=0;ih[12]=0;
        chunk(stdout,"IHDR",ih,13); chunk(stdout,"IDAT",comp,(uint32_t)cl); chunk(stdout,"IEND",NULL,0);
        frames++;
    }
    fflush(stdout);
    double dur=now()-t_start;
    fprintf(stderr,"drmrec: %ld frames in %.1fs (%.1f fps)\n",frames,dur,frames/dur);
    return 0;
}
