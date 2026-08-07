/* drmq - query the active DRM scanout framebuffer(s): size, format, modifier.
 * Tells us whether the screen buffer is LINEAR (easy CPU grab) or tiled/AFBC.
 * Links the device's libdrm.so.2 (vendored minimal decls).
 */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

typedef struct _drmModeRes { int count_fbs; uint32_t*fbs; int count_crtcs; uint32_t*crtcs;
    int count_connectors; uint32_t*connectors; int count_encoders; uint32_t*encoders;
    uint32_t min_width,max_width,min_height,max_height; } drmModeRes, *drmModeResPtr;
typedef struct _drmModePlaneRes { uint32_t count_planes; uint32_t*planes; } drmModePlaneRes, *drmModePlaneResPtr;
typedef struct _drmModePlane { uint32_t count_formats; uint32_t*formats; uint32_t plane_id, crtc_id, fb_id,
    crtc_x, crtc_y, x, y, possible_crtcs, gamma_size; } drmModePlane, *drmModePlanePtr;
typedef struct _drmModeFB2 { uint32_t fb_id, width, height, pixel_format; uint64_t modifier;
    uint32_t flags; uint32_t handles[4]; uint32_t pitches[4]; uint32_t offsets[4]; } drmModeFB2, *drmModeFB2Ptr;

extern drmModeResPtr drmModeGetResources(int fd);
extern drmModePlaneResPtr drmModeGetPlaneResources(int fd);
extern drmModePlanePtr drmModeGetPlane(int fd, uint32_t plane_id);
extern drmModeFB2Ptr drmModeGetFB2(int fd, uint32_t fb_id);
extern int drmSetClientCap(int fd, uint64_t cap, uint64_t val);

#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#define DRM_CLIENT_CAP_ATOMIC 3

static void fourcc(uint32_t f, char*o){ o[0]=f&0xff; o[1]=(f>>8)&0xff; o[2]=(f>>16)&0xff; o[3]=(f>>24)&0xff; o[4]=0; }

int main(int argc, char**argv){
    const char*dev = argc>1?argv[1]:"/dev/dri/card0";
    int fd = open(dev, O_RDWR);
    if(fd<0){ perror("open"); return 1; }
    printf("opened %s\n", dev);
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

    drmModePlaneResPtr pr = drmModeGetPlaneResources(fd);
    if(!pr){ printf("no plane resources\n"); return 1; }
    printf("planes: %u\n", pr->count_planes);
    for(uint32_t i=0;i<pr->count_planes;i++){
        drmModePlanePtr pl = drmModeGetPlane(fd, pr->planes[i]);
        if(!pl) continue;
        if(pl->fb_id){
            drmModeFB2Ptr fb = drmModeGetFB2(fd, pl->fb_id);
            if(fb){
                char cc[5]; fourcc(fb->pixel_format, cc);
                printf("PLANE %u crtc=%u fb=%u  %ux%u  fmt=%s(0x%08x)  modifier=0x%016llx  pitch0=%u  handle0=%u offset0=%u\n",
                    pl->plane_id, pl->crtc_id, pl->fb_id, fb->width, fb->height, cc, fb->pixel_format,
                    (unsigned long long)fb->modifier, fb->pitches[0], fb->handles[0], fb->offsets[0]);
            } else {
                printf("PLANE %u fb=%u  (drmModeGetFB2 failed - errno maybe perms)\n", pl->plane_id, pl->fb_id);
            }
        }
    }
    return 0;
}
