/* drmshot - grab the MPC X screen from the DRM scanout framebuffer -> PNG.
 * Primary plane is linear XRGB8888 (800x1280); we prime-export it, mmap for CPU
 * read, rotate, and write a 24-bit RGB PNG (deflate via device libz).
 * Usage: drmshot <out.png> [rotation 0|90|180|270] [/dev/dri/card0]
 */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

/* ---- minimal libdrm ---- */
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
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#define DRM_CLIENT_CAP_ATOMIC 3
struct dma_buf_sync { uint64_t flags; };
#define DMA_BUF_SYNC_READ  (1<<0)
#define DMA_BUF_SYNC_END   (1<<2)
#define DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct dma_buf_sync)

/* ---- minimal zlib ---- */
typedef struct z_stream_s {
    unsigned char *next_in; unsigned int avail_in; unsigned long total_in;
    unsigned char *next_out; unsigned int avail_out; unsigned long total_out;
    char *msg; void *state;
    void *(*zalloc)(void*,unsigned int,unsigned int); void (*zfree)(void*,void*); void *opaque;
    int data_type; unsigned long adler; unsigned long reserved;
} z_stream;
extern int deflateInit2_(z_stream*,int,int,int,int,int,const char*,int);
extern int deflate(z_stream*,int);
extern int deflateEnd(z_stream*);
extern unsigned long deflateBound(z_stream*,unsigned long);
extern unsigned long crc32(unsigned long,const unsigned char*,unsigned int);
#define Z_FINISH 4

static void put_be32(unsigned char*b,uint32_t v){ b[0]=v>>24;b[1]=v>>16;b[2]=v>>8;b[3]=v; }
static void write_chunk(FILE*f,const char*type,const unsigned char*data,uint32_t len){
    unsigned char h[4]; put_be32(h,len); fwrite(h,1,4,f); fwrite(type,1,4,f);
    if(len) fwrite(data,1,len,f);
    unsigned long crc=crc32(0,(const unsigned char*)type,4);
    if(len) crc=crc32(crc,data,len);
    unsigned char c[4]; put_be32(c,(uint32_t)crc); fwrite(c,1,4,f);
}

int main(int argc,char**argv){
    if(argc<2){ fprintf(stderr,"usage: %s out.png [rot 0|90|180|270] [card]\n",argv[0]); return 2; }
    const char*out=argv[1];
    int rot = argc>2?atoi(argv[2]):270;
    const char*dev = argc>3?argv[3]:"/dev/dri/card0";

    int fd=open(dev,O_RDWR); if(fd<0){ perror("open card"); return 1; }
    drmSetClientCap(fd,DRM_CLIENT_CAP_UNIVERSAL_PLANES,1);
    drmSetClientCap(fd,DRM_CLIENT_CAP_ATOMIC,1);
    drmModePlaneResPtr pr=drmModeGetPlaneResources(fd);
    uint32_t best_fb=0; uint64_t best_area=0;
    for(uint32_t i=0;pr&&i<pr->count_planes;i++){
        drmModePlanePtr pl=drmModeGetPlane(fd,pr->planes[i]);
        if(pl&&pl->fb_id){ drmModeFB2Ptr fb=drmModeGetFB2(fd,pl->fb_id);
            if(fb){ uint64_t a=(uint64_t)fb->width*fb->height; if(a>best_area){best_area=a;best_fb=pl->fb_id;} } }
    }
    if(!best_fb){ fprintf(stderr,"no framebuffer plane found\n"); return 1; }
    drmModeFB2Ptr fb=drmModeGetFB2(fd,best_fb);
    uint32_t W=fb->width,H=fb->height,pitch=fb->pitches[0];
    int primefd=-1;
    if(drmPrimeHandleToFD(fd,fb->handles[0],O_RDONLY,&primefd)!=0||primefd<0){ perror("PrimeHandleToFD"); return 1; }
    size_t sz=(size_t)pitch*H;
    unsigned char*map=mmap(NULL,sz,PROT_READ,MAP_SHARED,primefd,0);
    if(map==MAP_FAILED){ perror("mmap"); return 1; }
    struct dma_buf_sync ss={DMA_BUF_SYNC_READ}; ioctl(primefd,DMA_BUF_IOCTL_SYNC,&ss);

    uint32_t OW,OH; if(rot==90||rot==270){ OW=H; OH=W; } else { OW=W; OH=H; }

    /* build PNG raw scanlines: per row = filter(0) + RGB pixels (top-to-bottom) */
    size_t rowlen=1+(size_t)OW*3, rawlen=rowlen*OH;
    unsigned char*raw=malloc(rawlen); if(!raw){ fprintf(stderr,"oom\n"); return 1; }
    size_t q=0;
    for(uint32_t oy=0; oy<OH; oy++){
        raw[q++]=0;
        for(uint32_t ox=0; ox<OW; ox++){
            uint32_t sx,sy;
            if(rot==0){ sx=ox; sy=oy; }
            else if(rot==180){ sx=W-1-ox; sy=H-1-oy; }
            else if(rot==90){ sx=oy; sy=H-1-ox; }
            else { sx=W-1-oy; sy=ox; }              /* 270 */
            unsigned char*p=map+(size_t)sy*pitch+(size_t)sx*4; /* XRGB LE: B,G,R,X */
            raw[q++]=p[2]; raw[q++]=p[1]; raw[q++]=p[0];       /* R,G,B */
        }
    }
    struct dma_buf_sync se={DMA_BUF_SYNC_END|DMA_BUF_SYNC_READ}; ioctl(primefd,DMA_BUF_IOCTL_SYNC,&se);
    munmap(map,sz); close(primefd); close(fd);

    /* deflate the raw stream (zlib) */
    z_stream zs; memset(&zs,0,sizeof zs);
    if(deflateInit2_(&zs,6,8,15,8,0,"1.3.1",(int)sizeof(z_stream))!=0){ fprintf(stderr,"deflateInit failed\n"); return 1; }
    unsigned long bound=deflateBound(&zs,rawlen);
    unsigned char*comp=malloc(bound); if(!comp){ fprintf(stderr,"oom2\n"); return 1; }
    zs.next_in=raw; zs.avail_in=(unsigned int)rawlen; zs.next_out=comp; zs.avail_out=(unsigned int)bound;
    deflate(&zs,Z_FINISH); unsigned long complen=zs.total_out; deflateEnd(&zs);

    FILE*o=fopen(out,"wb"); if(!o){ perror("fopen out"); return 1; }
    static const unsigned char sig[8]={0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a}; fwrite(sig,1,8,o);
    unsigned char ihdr[13]; put_be32(ihdr,OW); put_be32(ihdr+4,OH);
    ihdr[8]=8; ihdr[9]=2; ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;  /* 8bit, RGB, no interlace */
    write_chunk(o,"IHDR",ihdr,13);
    write_chunk(o,"IDAT",comp,(uint32_t)complen);
    write_chunk(o,"IEND",NULL,0);
    fclose(o);
    free(raw); free(comp);
    fprintf(stderr,"wrote %s (%ux%u PNG, %lu bytes deflated, rot=%d)\n",out,OW,OH,complen,rot);
    return 0;
}
