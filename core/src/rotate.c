#include "fasteink.h"

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

/* Exact quarter-turn copy for the fixed-orientation panel. Small tiles keep both reads and
 * writes local; Android Canvas's general software transform is much slower. */
int fe_rotate(fe_image source, uint8_t *target, size_t target_pitch, int target_width,
              int target_height, int quarter, int left, int top, int right, int bottom) {
    if (quarter<0 || quarter>3 || !source.pixels || !target
            || target_width!=((quarter&1)?source.height:source.width)
            || target_height!=((quarter&1)?source.width:source.height)
            || left<0 || top<0 || right<=left || bottom<=top || right>target_width || bottom>target_height)
        return -1;
    const uint8_t *input=source.pixels;
    int width=source.width,height=source.height;
    size_t pitch=source.pitch;
#if defined(__ARM_NEON)
    int full_right=left+(right-left)/4*4,full_bottom=top+(bottom-top)/4*4;
    // Transpose four packed pixels at a time. Scalar strided copies spend most
    // of a landscape frame on address arithmetic and one-pixel memory accesses.
    for(int y=top;y<full_bottom;y+=4)for(int x=left;x<full_right;x+=4) {
        int sx=quarter==1?y:quarter==2?width-x-4:quarter==3?width-y-4:x;
        int sy=quarter==1?height-x-4:quarter==2?height-y-4:quarter==3?x:y;
        const uint8_t *base=input+(size_t)sy*pitch+(size_t)sx*4;
        uint32x4_t a=vld1q_u32((const uint32_t *)base);
        uint32x4_t b=vld1q_u32((const uint32_t *)(base+pitch));
        uint32x4_t c=vld1q_u32((const uint32_t *)(base+2*pitch));
        uint32x4_t d=vld1q_u32((const uint32_t *)(base+3*pitch));
        uint32x4_t rows[4];
        if(quarter&1) {
            uint32x4x2_t ab=vtrnq_u32(a,b),cd=vtrnq_u32(c,d);
            rows[0]=vcombine_u32(vget_low_u32(ab.val[0]),vget_low_u32(cd.val[0]));
            rows[1]=vcombine_u32(vget_low_u32(ab.val[1]),vget_low_u32(cd.val[1]));
            rows[2]=vcombine_u32(vget_high_u32(ab.val[0]),vget_high_u32(cd.val[0]));
            rows[3]=vcombine_u32(vget_high_u32(ab.val[1]),vget_high_u32(cd.val[1]));
            if(quarter==3) {
                uint32x4_t swap=rows[0];rows[0]=rows[3];rows[3]=swap;
                swap=rows[1];rows[1]=rows[2];rows[2]=swap;
            }
        } else {
            rows[0]=quarter==2?d:a;rows[1]=quarter==2?c:b;
            rows[2]=quarter==2?b:c;rows[3]=quarter==2?a:d;
        }
        for(int row=0;row<4;row++) {
            uint32x4_t pixels=rows[row];
            if(quarter==1 || quarter==2) {
                pixels=vrev64q_u32(pixels);pixels=vextq_u32(pixels,pixels,2);
            }
            vst1q_u32((uint32_t *)(target+(size_t)(y+row)*target_pitch)+(size_t)x,pixels);
        }
    }
#else
    // Desktop builds (core tests) copy every pixel below.
    int full_right=left,full_bottom=top;
#endif
    // Preserve exact clipping for the remaining one-to-three edge rows/columns.
    for(int y=top;y<bottom;y++)for(int x=y<full_bottom?full_right:left;x<right;x++) {
        int sx=quarter==1?y:quarter==2?width-1-x:quarter==3?width-1-y:x;
        int sy=quarter==1?height-1-x:quarter==2?height-1-y:quarter==3?x:y;
        *(uint32_t *)(target+(size_t)y*target_pitch+(size_t)x*4)=
                *(const uint32_t *)(input+(size_t)sy*pitch+(size_t)sx*4);
    }
    return 0;
}
