// libnpu_ioctl_tracer — LD_PRELOAD shim for the darwinn HAL. Hooks ioctl()
// and logs EDGETPU_* commands plus their args & return values. Most
// important: EDGETPU_MAP_DMABUF (0xc028ed11) — captures fd → NPU virt addr,
// so we can later replicate the exact allocation sequence in our standalone
// runner.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define LOG_PATH "/data/local/tmp/npu_ioctl_trace.txt"

#define EDGETPU_CREATE_GROUP    0x4014ed06
#define EDGETPU_FINALIZE_GROUP  0xed08
#define EDGETPU_SET_EVENTFD     0x4008ed05
#define EDGETPU_MAP_DMABUF      0xc028ed11
#define EDGETPU_RIO_SUBMIT      0xc050ed23
#define EDGETPU_POLL            0xc018ed24
#define EDGETPU_RELEASE_WAKE    0xed19
#define EDGETPU_ACQUIRE_WAKE    0xed1a
#define EDGETPU_BUF_REGISTER    0xc028ed11

struct map_dmabuf_in {
    uint64_t offset, size, device_address;
    int dmabuf_fd; uint32_t flags, die_index;
};

static int g_log_fd = -1;
// Match bionic prototype: int ioctl(int fd, int req, ...)
static int (*real_ioctl)(int, int, ...) = NULL;

static void open_log(void) {
    if (g_log_fd < 0)
        g_log_fd = open(LOG_PATH, O_WRONLY|O_CREAT|O_APPEND, 0644);
}
static void log_str(const char* s) {
    open_log();
    if (g_log_fd >= 0) write(g_log_fd, s, strlen(s));
}
static void log_fmt(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        open_log();
        if (g_log_fd >= 0) write(g_log_fd, buf, n);
    }
}

int ioctl(int fd, int req, ...) {
    va_list ap; va_start(ap, req);
    void* arg = va_arg(ap, void*);
    va_end(ap);

    if (!real_ioctl)
        real_ioctl = dlsym(RTLD_NEXT, "ioctl");

    // Cast to unsigned for matching ioctl numbers that have the high bit set.
    unsigned int ureq = (unsigned int)req;

    // Snapshot arg before the call (for MAP_DMABUF we want to see device_address
    // returned by the kernel).
    struct map_dmabuf_in snap;
    if (ureq == EDGETPU_MAP_DMABUF && arg) {
        memcpy(&snap, arg, sizeof(snap));
    }

    int rc = real_ioctl(fd, req, arg);
    int saved_errno = errno;

    // Filter: only log edgetpu-ish ioctls (those starting with 0xed in middle).
    int is_edgetpu = ((ureq >> 8) & 0xFF) == 0xed;
    if (is_edgetpu) {
        if (ureq == EDGETPU_MAP_DMABUF && arg) {
            struct map_dmabuf_in* p = (struct map_dmabuf_in*)arg;
            log_fmt("MAP_DMABUF fd_arg=%d size=%lu in_dev=0x%lx -> rc=%d "
                    "out_dev=0x%lx errno=%d\n",
                    snap.dmabuf_fd, (unsigned long)snap.size,
                    (unsigned long)snap.device_address,
                    rc, (unsigned long)p->device_address, saved_errno);
        } else if (ureq == EDGETPU_RIO_SUBMIT && arg) {
            uint8_t* b = (uint8_t*)arg;
            log_fmt("RIO_SUBMIT bytes[0..16]=", 0);
            char hb[64]; int n = 0;
            for (int i = 0; i < 16; i++)
                n += snprintf(hb+n, sizeof(hb)-n, "%02x ", b[i]);
            log_str(hb);
            uint64_t addr = *(uint64_t*)(b + 16);
            log_fmt(" addr=0x%lx rc=%d\n", (unsigned long)addr, rc);
        } else if (ureq == EDGETPU_CREATE_GROUP) {
            log_fmt("CREATE_GROUP rc=%d\n", rc);
        } else if (ureq == EDGETPU_FINALIZE_GROUP) {
            log_fmt("FINALIZE_GROUP rc=%d\n", rc);
        } else if (ureq == EDGETPU_SET_EVENTFD && arg) {
            uint32_t* e = (uint32_t*)arg;
            log_fmt("SET_EVENTFD event_id=%u eventfd=%u rc=%d\n",
                    e[0], e[1], rc);
        } else if (ureq == EDGETPU_ACQUIRE_WAKE) {
            log_fmt("ACQUIRE_WAKE rc=%d\n", rc);
        } else if (ureq == EDGETPU_RELEASE_WAKE) {
            log_fmt("RELEASE_WAKE rc=%d\n", rc);
        } else if (ureq == EDGETPU_POLL) {
            log_fmt("POLL rc=%d\n", rc);
        } else {
            log_fmt("ETPU req=0x%x fd=%d arg=%p rc=%d\n",
                    ureq, fd, arg, rc);
        }
    }
    errno = saved_errno;
    return rc;
}

__attribute__((constructor))
static void init_tracer(void) {
    int fd = open(LOG_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd >= 0) {
        char buf[64];
        int n = snprintf(buf, sizeof(buf),
                         "[npu_ioctl_tracer] init pid=%d\n", getpid());
        write(fd, buf, n);
        close(fd);
    }
}
