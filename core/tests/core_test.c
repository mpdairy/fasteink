/* Desktop checks of the core against a fake driver: scripts/test_core.sh. */
#include "fasteink.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(condition) do { if (!(condition)) { failures++; \
    fprintf(stderr,"%s:%d: failed: %s\n",__FILE__,__LINE__,#condition); } } while (0)

typedef struct {
    int32_t info[32];
    uint8_t *pixels;
    size_t mapped;
    int update_result, updates, closed;
    fe_update last;
} Fake;

static int fake_query(void *ctx, int32_t info[32]) { memcpy(info,((Fake *)ctx)->info,sizeof(((Fake *)ctx)->info)); return 0; }
static uint8_t *fake_map(void *ctx, size_t length) {
    Fake *f=ctx; f->pixels=calloc(1,length); f->mapped=length; return f->pixels;
}
static void fake_unmap(void *ctx, uint8_t *pixels, size_t length) { (void)ctx; (void)length; free(pixels); }
static int fake_update(void *ctx, const fe_update *update) {
    Fake *f=ctx; f->last=*update; f->updates++; return f->update_result;
}
static void fake_close(void *ctx) { ((Fake *)ctx)->closed++; }

static fe_driver driver(Fake *f, int width, int height) {
    memset(f,0,sizeof(*f));
    f->info[1]=width|height<<16; f->info[2]=width; f->info[3]=width*height;
    return (fe_driver){.ctx=f,.query=fake_query,.map=fake_map,.unmap=fake_unmap,.update=fake_update,.close=fake_close};
}

static const long long OLD=2606141001LL, NEW=2610091001LL;

static uint8_t *image(int width, int height, uint8_t gray) {
    uint8_t *pixels=malloc((size_t)width*height*4);
    for (int i=0;i<width*height;i++) { pixels[i*4]=pixels[i*4+1]=pixels[i*4+2]=gray; pixels[i*4+3]=255; }
    return pixels;
}
static void set(uint8_t *pixels, int width, int x, int y, uint8_t gray) {
    uint8_t *p=pixels+((size_t)y*width+x)*4; p[0]=p[1]=p[2]=gray;
}

static void firmware(void) {
    CHECK(fe_firmware_stamp("Chauvet.E103.2606141001.2389_release")==2606141001LL);
    CHECK(fe_firmware_stamp("Chauvet.E103.2610091001.2545_release")==NEW);
    CHECK(fe_firmware_stamp("Chauvet.E103.26100910012.2545_release")==0);
    CHECK(fe_firmware_stamp("unknown")==0);
    CHECK(fe_firmware_stamp(NULL)==0);
}

static void probe(void) {
    Fake f; fe_caps caps;
    fe_driver manta=driver(&f,1920,2560);
    CHECK(fe_probe(&manta,OLD,&caps)==0 && caps.width==1920 && caps.height==2560 && caps.binary_plane==-1);
    CHECK(fe_probe(&manta,NEW,&caps)==0 && caps.binary_plane==0);
    fe_driver nomad=driver(&f,1872,1404);
    CHECK(fe_probe(&nomad,OLD,&caps)==0 && caps.width==1872 && caps.height==1404 && caps.binary_plane==1);
    CHECK(fe_probe(&nomad,NEW,&caps)==0 && caps.binary_plane==0);
    fe_driver other=driver(&f,1000,1000);
    CHECK(fe_probe(&other,NEW,&caps)==-ENODEV);
    // A known size with another stride is still unknown.
    nomad=driver(&f,1872,1404); f.info[2]=1888;
    CHECK(fe_probe(&nomad,NEW,&caps)==-ENODEV);
}

static void gray_session(void) {
    Fake f; fe_error error={0};
    uint8_t *pixels=image(10,8,255);
    fe_image page={pixels,40,10,8};
    fe_session *s=fe_open(driver(&f,1920,2560),FE_GRAY,NEW,100,200,page,&error);
    CHECK(s!=NULL);
    CHECK(f.mapped>=(size_t)1920*2560*3);
    CHECK(fe_present(s,page,0,0,10,8,0,&error)==FE_UNCHANGED && f.updates==0);
    set(pixels,10,3,4,0x80);
    CHECK(fe_present(s,page,2,2,6,6,0,&error)==FE_PRESENTED && f.updates==1);
    CHECK(f.last.left==102 && f.last.top==202 && f.last.right==106 && f.last.bottom==206);
    CHECK(f.last.mode==7 && f.last.flags==0 && f.last.offset==0);
    CHECK(fe_read_gray(s,103,204)==0x8 && fe_read_gray(s,102,202)==0xf);
    // The whole region is written, not just the changed pixel.
    CHECK(f.pixels[(size_t)205*1920+105]==0xf);
    CHECK(fe_present(s,page,2,2,6,6,0,&error)==FE_UNCHANGED);
    CHECK(fe_present(s,page,2,2,6,6,1,&error)==FE_PRESENTED && f.updates==2);
    // Busy keeps the region changed for the next sample.
    set(pixels,10,0,0,0);
    f.update_result=-EAGAIN;
    CHECK(fe_present(s,page,0,0,1,1,0,&error)==FE_BUSY);
    f.update_result=0;
    CHECK(fe_present(s,page,0,0,1,1,0,&error)==FE_PRESENTED);
    f.update_result=-EIO;
    CHECK(fe_present(s,page,0,0,1,1,1,&error)==FE_FAILED && error.message==NULL && error.errnum==EIO);
    f.update_result=0;
    CHECK(fe_present(s,page,0,0,11,1,0,&error)==FE_FAILED && error.message!=NULL);
    fe_image other={pixels,40,9,8};
    CHECK(fe_present(s,other,0,0,1,1,0,&error)==FE_FAILED);
    fe_close(s);
    CHECK(f.closed==1);
    // A patch that would leave the buffer is refused, and the driver closed.
    CHECK(fe_open(driver(&f,1920,2560),FE_GRAY,NEW,1915,0,page,&error)==NULL && f.closed==1);
    free(pixels);
}

static void binary_session(void) {
    Fake f; fe_error error={0};
    uint8_t *pixels=image(4,4,255);
    fe_image control={pixels,16,4,4};
    CHECK(fe_open(driver(&f,1920,2560),FE_FAST_BINARY,OLD,0,0,control,&error)==NULL && f.closed==1);
    fe_session *s=fe_open(driver(&f,1872,1404),FE_FAST_BINARY,OLD,10,10,control,&error);
    CHECK(s!=NULL);
    set(pixels,4,1,1,0);
    CHECK(fe_present(s,control,0,0,4,4,0,&error)==FE_PRESENTED);
    CHECK(f.last.mode==9 && f.last.flags==1 && f.last.offset==1872*1404);
    CHECK(fe_read_gray(s,11,11)==0);
    set(pixels,4,2,2,0x80);
    int before=f.updates;
    CHECK(fe_present(s,control,0,0,4,4,0,&error)==FE_FAILED && error.message!=NULL && f.updates==before);
    // Only the submitted region needs to be binary.
    CHECK(fe_present(s,control,0,0,2,2,1,&error)==FE_PRESENTED);
    fe_close(s);
    s=fe_open(driver(&f,1872,1404),FE_FAST_BINARY,NEW,0,0,control,&error);
    set(pixels,4,2,2,0);
    CHECK(s && fe_present(s,control,0,0,4,4,0,&error)==FE_PRESENTED && f.last.offset==0);
    fe_close(s);
    s=fe_open(driver(&f,1872,1404),FE_GRAY_FLAGGED,NEW,0,0,control,&error);
    set(pixels,4,2,2,0x80);
    CHECK(s && fe_present(s,control,0,0,4,4,0,&error)==FE_PRESENTED && f.last.mode==7 && f.last.flags==1);
    fe_close(s);
    free(pixels);
}

static void rotation(void) {
    // A 5x3 source with distinct pixels, turned like Android's Matrix.setRotate(turn*90).
    int width=5,height=3;
    uint32_t source[15],target[15];
    for (int i=0;i<15;i++) source[i]=0x1000u+i;
    fe_image image={(const uint8_t *)source,(size_t)width*4,width,height};
    for (int quarter=0;quarter<4;quarter++) {
        int tw=(quarter&1)?height:width, th=(quarter&1)?width:height;
        memset(target,0,sizeof(target));
        CHECK(fe_rotate(image,(uint8_t *)target,(size_t)tw*4,tw,th,quarter,0,0,tw,th)==0);
        for (int y=0;y<height;y++) for (int x=0;x<width;x++) {
            // Clockwise turns of the source point (x,y).
            int tx=quarter==0?x:quarter==1?height-1-y:quarter==2?width-1-x:y;
            int ty=quarter==0?y:quarter==1?x:quarter==2?height-1-y:width-1-x;
            CHECK(target[ty*tw+tx]==source[y*width+x]);
        }
    }
    // Only the requested region is written.
    memset(target,0,sizeof(target));
    CHECK(fe_rotate(image,(uint8_t *)target,(size_t)3*4,3,5,1,1,1,2,3)==0);
    for (int y=0;y<5;y++) for (int x=0;x<3;x++) CHECK((target[y*3+x]!=0)==(x==1 && y>=1 && y<3));
    CHECK(fe_rotate(image,(uint8_t *)target,(size_t)5*4,5,3,1,0,0,1,1)==-1);
}

int main(void) {
    firmware(); probe(); gray_session(); binary_session(); rotation();
    if (failures) { fprintf(stderr,"%d core checks failed\n",failures); return 1; }
    puts("Core checks passed");
    return 0;
}
