#include "fasteink.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(fe_update)==24,"HT region ABI");

/*
 * Known driver buffers. Restrict updates to these, never guess an ABI from Android's model
 * property (the Manta incorrectly says Nomad). Supporting another panel means adding a row
 * here, observed on the device itself; see docs/DEVICES.md.
 */
typedef struct {
    int width, height, stride, size;
    /* The plane for FE_FAST_BINARY (mode 9/flags 1) on firmware before and from
       PEN_MAIN_PLANE_SINCE, or -1 when binary feedback stays on the gray path. */
    int older_binary_plane, newer_binary_plane;
} Layout;
static const Layout LAYOUTS[]={
    // Manta: the buffer matches the portrait screen. Mode 9 on the main plane from
    // 3.30.45, confirmed by hand 2026-10-12.
    {1920,2560,1920,4915200,-1,0},
    // Nomad: the panel's landscape scan order (firmware hwrota=270), observed from the
    // shared plane on 2026-10-05. Plane 1 leaves black boxes from 3.30.45 on.
    {1872,1404,1872,2628288,1,0},
};
/*
 * The first build whose libeinkutils.so sends the firmware's own pen requests (mode 9) to the
 * main plane rather than the plane after it. Its gray frames also queue behind the pen on the
 * Manta, as they always did on the Nomad. Found on 3.30.45 (2026-10-12).
 */
static const long long PEN_MAIN_PLANE_SINCE=2610091001LL;

static const Layout *supported_layout(const int32_t *info) {
    int width=(uint32_t)info[1]&0xffff, height=(uint32_t)info[1]>>16;
    for (size_t i=0;i<sizeof(LAYOUTS)/sizeof(*LAYOUTS);i++) {
        const Layout *l=&LAYOUTS[i];
        if (l->width==width && l->height==height && l->stride==info[2] && l->size==info[3]) return l;
    }
    return NULL;
}

static int binary_plane(const Layout *layout, long long firmware_stamp) {
    return firmware_stamp>=PEN_MAIN_PLANE_SINCE ? layout->newer_binary_plane : layout->older_binary_plane;
}

long long fe_firmware_stamp(const char *build) {
    // The first run of exactly ten digits between dots.
    for (const char *p=build ? strchr(build,'.') : NULL; p; p=strchr(p+1,'.')) {
        int digits=0;
        while (p[1+digits]>='0' && p[1+digits]<='9') digits++;
        if (digits==10 && p[11]=='.') return strtoll(p+1,NULL,10);
    }
    return 0;
}

int fe_probe(const fe_driver *driver, long long firmware_stamp, fe_caps *out) {
    int32_t info[32]={0};
    int result=driver->query(driver->ctx,info);
    if (result!=0) return result;
    const Layout *l=supported_layout(info);
    if (!l) return -ENODEV;
    out->width=l->width; out->height=l->height;
    out->binary_plane=binary_plane(l,firmware_stamp);
    return 0;
}

void fe_describe(const fe_driver *driver, char *out, size_t size) {
    int32_t info[32]={0};
    int result=driver->query(driver->ctx,info);
    if (result!=0) snprintf(out,size,"Display info unavailable: %s",strerror(-result));
    else snprintf(out,size,"Display info: %d %d %d %d %d %d %d %d",
                  info[0],info[1],info[2],info[3],info[4],info[5],info[6],info[7]);
}

struct fe_session {
    fe_driver driver;
    uint8_t *pixels;
    size_t length;
    int width, height, stride;
    int offset;
    uint8_t *previous;
    int canvas_width, canvas_height, x, y;
    uint8_t request_flags, display_mode;
};

static int fail(fe_error *error, const char *message, int errnum) {
    if (error) { error->message=message; error->errnum=errnum; }
    return FE_FAILED;
}

fe_session *fe_open(fe_driver driver, fe_mode mode, long long firmware_stamp,
                    int x, int y, fe_image background, fe_error *error) {
    int32_t info[32]={0};
    int result=driver.query(driver.ctx,info);
    if (result!=0) { driver.close(driver.ctx); fail(error,NULL,-result); return NULL; }
    const Layout *layout=supported_layout(info);
    if (!layout) { driver.close(driver.ctx); fail(error,"Unsupported display buffer layout",0); return NULL; }
    // Only the request settings observed in installed display clients. Mode 9 accepts only
    // exact binary control pixels; mode 7 keeps gray drawing on plane 0.
    int display_mode=7, request_flags=0, plane=0;
    if (mode==FE_FAST_BINARY) {
        display_mode=9; request_flags=1; plane=binary_plane(layout,firmware_stamp);
        if (plane<0) {
            driver.close(driver.ctx);
            fail(error,"Fast binary controls are not supported on this firmware",0); return NULL;
        }
    } else if (mode==FE_GRAY_FLAGGED) request_flags=1;
    else if (mode!=FE_GRAY) { driver.close(driver.ctx); fail(error,"Unsupported display mode",0); return NULL; }
    int width=layout->width,height=layout->height;
    if (!background.pixels || background.width<=0 || background.height<=0
            || background.width>width || background.height>height
            || x<0 || y<0 || x>width-background.width || y>height-background.height) {
        driver.close(driver.ctx); fail(error,"Display background outside app canvas",0); return NULL;
    }
    size_t length=((size_t)info[3]*3+95)&~(size_t)63;
    uint8_t *pixels=driver.map(driver.ctx,length);
    if (!pixels) { int e=errno; driver.close(driver.ctx); fail(error,NULL,e); return NULL; }
    fe_session *d=calloc(1,sizeof(*d));
    uint8_t *previous=malloc((size_t)background.width*background.height);
    if (!d || !previous) {
        free(d); free(previous); driver.unmap(driver.ctx,pixels,length); driver.close(driver.ctx);
        fail(error,"Display allocation failed",0); return NULL;
    }
    d->driver=driver; d->pixels=pixels; d->length=length;
    d->width=width; d->height=height; d->stride=info[2];
    d->offset=plane*info[3];
    d->canvas_width=background.width; d->canvas_height=background.height; d->x=x; d->y=y;
    d->request_flags=(uint8_t)request_flags;
    d->display_mode=(uint8_t)display_mode;
    d->previous=previous;
    // Seed our private comparison copy. The compositor also uses the mapped plane, so a
    // one-time initialization of it would not stay authoritative. Each submitted region
    // must copy its own background/border in fe_present.
    for (int row=0;row<d->canvas_height;row++) {
        const uint8_t *src=background.pixels+(size_t)row*background.pitch;
        uint8_t *dst=d->previous+(size_t)row*d->canvas_width;
        for (int column=0;column<d->canvas_width;column++,src+=4) *dst++=src[0]>>4;
    }
    return d;
}

int fe_present(fe_session *d, fe_image image, int left, int top, int right, int bottom,
               int force, fe_error *error) {
    // Reject before writing. The entire app canvas must fit the screen.
    if (!d || !image.pixels || image.width!=d->canvas_width || image.height!=d->canvas_height
            || left<0 || top<0 || right<=left || bottom<=top
            || right>image.width || bottom>image.height)
        return fail(error,"Display region outside app canvas",0);
    // Do not threshold grays or submit partially converted data to the pen plane.
    // The caller can use normal Android presentation on rejection.
    if (d->display_mode==9) {
        for (int y=top;y<bottom;y++) {
            const uint8_t *src=image.pixels+(size_t)y*image.pitch+(size_t)left*4;
            for (int x=left;x<right;x++,src+=4) {
                if (src[3]!=255 || (src[0]!=0 && src[0]!=255) || src[1]!=src[0] || src[2]!=src[0])
                    return fail(error,"Fast control pixels must be opaque black or white",0);
            }
        }
    }
    int changed=force;
    for (int y=top;y<bottom && !changed;y++) {
        const uint8_t *src=image.pixels+(size_t)y*image.pitch+(size_t)left*4;
        const uint8_t *old=d->previous+(size_t)y*d->canvas_width+left;
        for (int x=left;x<right;x++,src+=4,old++) {
            if ((src[0]>>4)!=*old) { changed=1; break; }
        }
    }
    if (!changed) return FE_UNCHANGED;
    // Preserve the rasterizer's clean border. Tight changed-pixel cropping in 0.7 coincided
    // with physical horizontal/vertical edge artifacts. Keep no-op suppression, without
    // assuming single-pixel refresh bounds.
    for (int y=top;y<bottom;y++) {
        const uint8_t *src=image.pixels+(size_t)y*image.pitch+(size_t)left*4;
        uint8_t *dst=d->pixels+d->offset+(size_t)(y+d->y)*d->stride+left+d->x;
        for (int x=left;x<right;x++,src+=4) *dst++=src[0]>>4;
    }
    __sync_synchronize();
    fe_update update={.left=left+d->x,.top=top+d->y,.right=right+d->x,.bottom=bottom+d->y,
                      .offset=d->offset,.mode=d->display_mode,.flags=d->request_flags};
    int result=d->driver.update(d->driver.ctx,&update);
    if (result<0) {
        // Busy display queues apply backpressure.
        if (result==-EAGAIN || result==-EINTR) return FE_BUSY;
        return fail(error,NULL,-result);
    }
    for (int y=top;y<bottom;y++) {
        const uint8_t *src=image.pixels+(size_t)y*image.pitch+(size_t)left*4;
        uint8_t *old=d->previous+(size_t)y*d->canvas_width+left;
        for (int x=left;x<right;x++,src+=4) *old++=src[0]>>4;
    }
    return FE_PRESENTED;
}

int fe_read_gray(const fe_session *d, int x, int y) {
    if (!d || x<0 || y<0 || x>=d->width || y>=d->height) return -1;
    return d->pixels[d->offset+(size_t)y*d->stride+x];
}

void fe_close(fe_session *d) {
    if (!d) return;
    d->driver.unmap(d->driver.ctx,d->pixels,d->length);
    d->driver.close(d->driver.ctx);
    free(d->previous); free(d);
}
