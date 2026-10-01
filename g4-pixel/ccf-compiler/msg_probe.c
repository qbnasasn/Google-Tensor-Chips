// msg_probe.so — LD_PRELOAD software breakpoint.
// Patches a `brk #0` at a fixed file-offset inside libedgetpu_tflite_compiler.so
// (the entry of the "print exception message" helper, where x1 = raw message
// pointer, untouched by the caller). On SIGTRAP, dumps x0/x1 + the string at x1,
// restores the original 4 bytes, and lets execution continue normally.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include <fcntl.h>

#define LOG_PATH "/data/local/tmp/msg_probe_log.txt"
#define TARGET_OFFSET 0x39c8014UL

static uint8_t g_orig[4];
static uintptr_t g_addr = 0;
static int g_armed = 0;

static void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int fd = open(LOG_PATH, O_WRONLY|O_CREAT|O_APPEND, 0644);
    if (fd >= 0) { write(fd, buf, n); close(fd); }
}

static struct { const char* match; void* base; } g_lookup;
static int phdr_cb(struct dl_phdr_info* info, size_t size, void* data) {
    (void)size; (void)data;
    if (info->dlpi_name && strstr(info->dlpi_name, g_lookup.match)) {
        g_lookup.base = (void*)info->dlpi_addr;
        return 1;
    }
    return 0;
}

static void restore_orig(void) {
    uintptr_t page = g_addr & ~0xfffULL;
    mprotect((void*)page, 0x2000, PROT_READ|PROT_WRITE|PROT_EXEC);
    memcpy((void*)g_addr, g_orig, 4);
    __builtin___clear_cache((char*)g_addr, (char*)g_addr + 4);
    mprotect((void*)page, 0x2000, PROT_READ|PROT_EXEC);
}

static void trap_handler(int sig, siginfo_t* si, void* uc_v) {
    (void)sig; (void)si;
    ucontext_t* uc = (ucontext_t*)uc_v;
    uint64_t x0 = uc->uc_mcontext.regs[0];
    uint64_t x1 = uc->uc_mcontext.regs[1];
    logf("[msg_probe] TRAP hit. x0=0x%llx x1=0x%llx\n",
         (unsigned long long)x0, (unsigned long long)x1);

    // dump raw bytes at x1 (the message pointer), guarded loosely
    logf("[msg_probe] bytes @x1: ");
    unsigned char* p = (unsigned char*)(uintptr_t)x1;
    char hexbuf[16*3+1]; char asciibuf[17];
    for (int i = 0; i < 128; i += 16) {
        int any = 0;
        int hp = 0;
        for (int j = 0; j < 16; j++) {
            unsigned char c = p[i+j];
            hp += snprintf(hexbuf+hp, sizeof(hexbuf)-hp, "%02x ", c);
            asciibuf[j] = (c >= 32 && c < 127) ? c : '.';
            any = 1;
        }
        asciibuf[16] = 0;
        if (any) logf("\n  +%03d: %s  %s", i, hexbuf, asciibuf);
    }
    logf("\n");

    // one-shot: restore original instruction so the real call proceeds normally
    restore_orig();
}

static void arm_probe(void) {
    if (g_armed) return; // already armed
    g_lookup.match = "libedgetpu_tflite_compiler";
    g_lookup.base = NULL;
    dl_iterate_phdr(phdr_cb, NULL);
    if (!g_lookup.base) { logf("[msg_probe] base not found\n"); return; }

    g_addr = (uintptr_t)g_lookup.base + TARGET_OFFSET;
    memcpy(g_orig, (void*)g_addr, 4);
    logf("[msg_probe] base=%p addr=0x%lx orig=%02x %02x %02x %02x\n",
         g_lookup.base, g_addr, g_orig[0], g_orig[1], g_orig[2], g_orig[3]);

    uintptr_t page = g_addr & ~0xfffULL;
    if (mprotect((void*)page, 0x2000, PROT_READ|PROT_WRITE|PROT_EXEC) != 0) {
        logf("[msg_probe] mprotect FAIL\n"); return;
    }
    static const uint8_t brk[4] = {0x00, 0x00, 0x20, 0xd4}; // brk #0
    memcpy((void*)g_addr, brk, 4);
    __builtin___clear_cache((char*)g_addr, (char*)g_addr + 4);
    mprotect((void*)page, 0x2000, PROT_READ|PROT_EXEC);
    g_armed = 1;

    struct sigaction sa = {0};
    sa.sa_sigaction = trap_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTRAP, &sa, NULL);
    logf("[msg_probe] armed at 0x%lx, SIGTRAP handler installed\n", g_addr);
}

// Interpose dlsym: patch at the moment the driver resolves CompileTfliteFlatbuffer2,
// which guarantees the compiler .so is already loaded (LD_PRELOAD ctors run too early).
static void* (*real_dlsym)(void*, const char*) = NULL;
void* dlsym(void* handle, const char* symbol) {
    if (!real_dlsym) real_dlsym = (void*(*)(void*,const char*))dlvsym(RTLD_NEXT, "dlsym", "libdl.so");
    void* result = real_dlsym(handle, symbol);
    if (symbol && strcmp(symbol, "CompileTfliteFlatbuffer2") == 0) {
        logf("[msg_probe] dlsym(CompileTfliteFlatbuffer2) resolved -> arming probe\n");
        arm_probe();
    }
    return result;
}

__attribute__((constructor))
static void install_probe(void) {
    int fd = open(LOG_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd >= 0) close(fd);
    logf("[msg_probe] loaded, waiting for dlsym(CompileTfliteFlatbuffer2)\n");
}
