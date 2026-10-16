/*
 * fasteink core: direct region updates on e-ink panels, in plain C.
 *
 * No JNI and no Android here, so the core can be tested on a desktop against a
 * fake driver and linked by native apps. The Android library is a thin shim over it.
 */
#ifndef FASTEINK_H
#define FASTEINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What an update is for. Backends map each intent to their own driver modes. */
typedef enum {
    /* Normal gray drawing (Supernote mode 7). */
    FE_GRAY = 0,
    /* Pen feedback and control presses in exact black/white (Supernote mode 9). Its pixels
       persist through Android redraws, so whoever draws there must erase them. */
    FE_FAST_BINARY = 1,
    /* Diagnostic only: mode 7 with request flags 1, as some installed display clients send it. */
    FE_GRAY_FLAGGED = 2,
} fe_mode;

/* fe_present results. Failures that need the Android fallback are FE_FAILED. */
enum {
    FE_PRESENTED = 1,
    FE_UNCHANGED = 0,
    /* The display queue applied backpressure. Keep the damage and retry on the next sample;
       never sleep on the pen thread. */
    FE_BUSY = -1,
    FE_FAILED = -2,
};

/* Why a call failed: a fixed message, or an errno when message is NULL. */
typedef struct {
    const char *message;
    int errnum;
} fe_error;

/* The HT driver's region update ABI (installed Manta/Nomad firmware, confirmed against
   Atelier's region update). A device interface declaration, not vendor implementation code. */
typedef struct {
    int32_t left, top, right, bottom;
    int32_t offset;
    uint8_t mode, flags, reserved, padding;
} fe_update;

/* Driver access. fe_driver_open_ebc gives the real device; tests substitute a fake. */
typedef struct fe_driver {
    void *ctx;
    /* Fills the driver's info words; 0, or -errno. */
    int (*query)(void *ctx, int32_t info[32]);
    /* Maps the shared planes; NULL with errno set on failure. */
    uint8_t *(*map)(void *ctx, size_t length);
    void (*unmap)(void *ctx, uint8_t *pixels, size_t length);
    /* Submits a region; 0, or -errno. */
    int (*update)(void *ctx, const fe_update *update);
    void (*close)(void *ctx);
} fe_driver;

/* Opens /dev/ebc. 0, or -errno when the device is missing or denied. */
int fe_driver_open_ebc(fe_driver *out);

/* What the panel supports, from the driver's own answer. Never from a model string. */
typedef struct {
    /* The driver buffer, in its own scan order (landscape on a Nomad). */
    int width, height;
    /* The plane FE_FAST_BINARY draws on with this firmware, or -1 when it is unsupported. */
    int binary_plane;
} fe_caps;

/* The yyMMddHHmm stamp in a firmware build like "Chauvet.E103.2606141001.2389_release";
   0 when absent. */
long long fe_firmware_stamp(const char *build);

/* Queries the driver and matches a known layout. 0 when supported; -ENODEV for an unknown
   layout (never guess an ABI); another -errno when the query failed. Does not close driver. */
int fe_probe(const fe_driver *driver, long long firmware_stamp, fe_caps *out);

/* The driver's raw info words as one line, for bug reports. Does not close driver. */
void fe_describe(const fe_driver *driver, char *out, size_t size);

typedef struct fe_session fe_session;

/* RGBA_8888 pixels; the gray level is the red channel. */
typedef struct {
    const uint8_t *pixels;
    size_t pitch;
    int width, height;
} fe_image;

/*
 * Takes ownership of driver, closing it on failure too. The session owns a width x height
 * rectangle of the driver buffer at (x, y), with background as its starting pixels.
 */
fe_session *fe_open(fe_driver driver, fe_mode mode, long long firmware_stamp,
                    int x, int y, fe_image background, fe_error *error);

/*
 * Presents [left,right) x [top,bottom) of image, which has the session's size. Unless force,
 * an unchanged region is not sent. The whole region is sent, never cropped to the changed
 * pixels: tight cropping in MonoPaint 0.7 coincided with physical edge artifacts.
 */
int fe_present(fe_session *session, fe_image image, int left, int top, int right, int bottom,
               int force, fe_error *error);

/* The 4-bit gray at a driver buffer position on the session's plane; -1 outside it. */
int fe_read_gray(const fe_session *session, int x, int y);

void fe_close(fe_session *session);

/*
 * Copies [left,right) x [top,bottom) of the target from source turned by quarter * 90 degrees
 * clockwise. Exact pixel copies; target has source's size, swapped for odd turns. 0 when the
 * arguments are valid.
 */
int fe_rotate(fe_image source, uint8_t *target, size_t target_pitch, int target_width,
              int target_height, int quarter, int left, int top, int right, int bottom);

#ifdef __cplusplus
}
#endif

#endif
