#include "fasteink.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* The Supernote HT driver. Its requests are device interface declarations, not vendor code. */
#define EBC_INFO 0x48545201UL
#define EBC_REGION_UPDATE 0x48545701UL

static int fd_of(void *ctx) { return (int)(intptr_t)ctx; }

static int query(void *ctx, int32_t info[32]) {
    return ioctl(fd_of(ctx),EBC_INFO,info)==0 ? 0 : -errno;
}
static uint8_t *map(void *ctx, size_t length) {
    void *pixels=mmap(NULL,length,PROT_READ|PROT_WRITE,MAP_SHARED,fd_of(ctx),0);
    return pixels==MAP_FAILED ? NULL : pixels;
}
static void unmap(void *ctx, uint8_t *pixels, size_t length) { (void)ctx; munmap(pixels,length); }
static int update(void *ctx, const fe_update *region) {
    return ioctl(fd_of(ctx),EBC_REGION_UPDATE,region)>=0 ? 0 : -errno;
}
static void close_fd(void *ctx) { close(fd_of(ctx)); }

int fe_driver_open_ebc(fe_driver *out) {
    int fd=open("/dev/ebc",O_RDWR|O_CLOEXEC);
    if (fd<0) return -errno;
    *out=(fe_driver){.ctx=(void *)(intptr_t)fd,.query=query,.map=map,.unmap=unmap,.update=update,.close=close_fd};
    return 0;
}
