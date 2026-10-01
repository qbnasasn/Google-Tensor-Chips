// PixelG4A17_Compiler — V2 + die() patch + HW patches forcing case 16 (v6) for Pixel 9 / Tensor G4 on Android 17+.
//
// Adapted from the original on-device compiler driver RE work (formerly nicknamed probe6).
// DO NOT use old "probe6" name. This is specifically for current A17+ lib versions.
//
// Critical: patches below are for the Android 16 lib and WILL need updating for A17.
// Use this as the framework, then find new offsets by disassembling the current
// /vendor/lib64/libedgetpu_tflite_compiler.so  (search for version dispatcher / die).
//
// The HardwareVersion dispatcher should map case 16 -> v6 (the one with rio_a0 + v6_config in TOC).
// Force the two callers to feed 16 instead of whatever the lib reads.
//
// AArch64 encoding:
//   mov w20, #16  ->  0x52800214 (LE bytes: 14 02 80 52)
//   mov w0,  #16  ->  0x52800200 (LE bytes: 00 02 80 52)

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

typedef int (*compile2_fn_t)(int32_t, int64_t, const void*, int64_t,
                             int32_t, int*, int64_t*, char**);
typedef int (*get_version_fn_t)(void);

struct Patch {
    size_t off;
    uint8_t orig[4];
    uint8_t newb[4];
    const char* desc;
};

static struct Patch g_patches[] = {
    // A17 offsets — RE'd 2026-07-01 from the current A17 /vendor/lib64/libedgetpu_tflite_compiler.so.
    // HardwareVersion->config dispatcher get_config_by_version() @ 0xee9200 (byte jump-table @
    // 0x22f845): valid cases {0,3,4,5,11,14,16,24,26,31}, and w0=16 -> case 0xee92e0 -> v6_config
    // (rio_a0, the only config registered in the TOC). Exactly two callers (0x7eb408, 0x7f6f90)
    // load the HW version and pass it in w0; force both to #16. Orig bytes are byte-identical to
    // the A16 patches (same instructions, same struct offsets 0x50/0x8) — only relocated.
    // ldr w20,[x0,0x50] -> mov w20, #16   (caller bl@0x7eb408)
    { 0x7eb3f4, {0x14, 0x50, 0x40, 0xb9}, {0x14, 0x02, 0x80, 0x52},
      "HwVer load #1 -> mov w20,#16 (v6) @ 0x7eb3f4" },
    // ldr w0,[x21,8]   -> mov w0, #16     (caller bl@0x7f6f90)
    { 0x7f6f88, {0xa0, 0x0a, 0x40, 0xb9}, {0x00, 0x02, 0x80, 0x52},
      "HwVer load #2 -> mov w0,#16 (v6)  @ 0x7f6f88" },
    // die() entry (str x30,[sp,-0x10]!; mov w0,#1; bl _exit@plt) -> ret
    { 0x7fdb88, {0xfe, 0x0f, 0x1f, 0xf8}, {0xc0, 0x03, 0x5f, 0xd6},
      "die() -> ret @ 0x7fdb88" },
};
#define N_PATCHES (sizeof(g_patches)/sizeof(g_patches[0]))

static int patch_in_memory(void* base, size_t off, const uint8_t* expected,
                           const uint8_t* new_bytes, size_t n) {
    uintptr_t addr = (uintptr_t)base + off;
    uintptr_t page = addr & ~0xfffULL;
    size_t plen = ((addr + n + 0xfff) & ~0xfffULL) - page;
    if (memcmp((void*)addr, expected, n) != 0) {
        printf("  patch: ORIG mismatch at 0x%zx — got: ", off);
        for (size_t i = 0; i < n; i++) printf("%02x ", ((uint8_t*)addr)[i]);
        printf("\n");
        return -1;
    }
    if (mprotect((void*)page, plen, PROT_READ|PROT_WRITE|PROT_EXEC) != 0) {
        printf("  mprotect RW FAIL\n"); return -1;
    }
    memcpy((void*)addr, new_bytes, n);
    __builtin___clear_cache((char*)addr, (char*)addr + n);
    mprotect((void*)page, plen, PROT_READ|PROT_EXEC);
    return 0;
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

static int get_int_property(const char* name, int default_val) {
    char buf[PROP_VALUE_MAX];
    int n = __system_property_get(name, buf);
    if (n <= 0) return default_val;
    char* end = NULL;
    long v = strtol(buf, &end, 10);
    return (end == buf) ? default_val : (int)v;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("════ PixelG4A17_Compiler — V2 + force HW=16 (v6) for A17+ ════\n");

    const char* tflite_path = argc > 1 ? argv[1]
                                       : "/data/local/tmp/embgemma_seq512.tflite";
    const char* options_path = argc > 2 ? argv[2]
                                        : "/data/local/tmp/pixelg4a17_options.bin";
    const char* compiler_path = "/vendor/lib64/libedgetpu_tflite_compiler.so";
    int hw_soc_rev = get_int_property("ro.boot.hw.soc.rev", 1);

    printf("  hw_soc_rev=%d (passed as V2 arg-4 = uid)\n\n", hw_soc_rev);

    struct rlimit rl = { (rlim_t)8ULL << 30, (rlim_t)8ULL << 30 };
    setrlimit(RLIMIT_DATA, &rl);
    cpu_set_t mask; CPU_ZERO(&mask);
    for (int i = 4; i < 8; i++) CPU_SET(i, &mask);
    sched_setaffinity(0, sizeof(mask), &mask);

    void* h = dlopen(compiler_path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { printf("dlopen FAIL: %s\n", dlerror()); return 1; }
    printf("  [1] dlopen OK\n");

    g_lookup.match = "libedgetpu_tflite_compiler"; g_lookup.base = NULL;
    dl_iterate_phdr(phdr_cb, NULL);
    if (!g_lookup.base) { printf("no base\n"); return 1; }
    printf("  load_base=%p\n", g_lookup.base);

    for (size_t i = 0; i < N_PATCHES; i++) {
        if (patch_in_memory(g_lookup.base, g_patches[i].off,
                            g_patches[i].orig, g_patches[i].newb, 4) == 0) {
            printf("  [2.%zu] patched: %s\n", i, g_patches[i].desc);
            // Read back & confirm
            uint8_t* a = (uint8_t*)g_lookup.base + g_patches[i].off;
            printf("        verify: %02x %02x %02x %02x\n",
                   a[0], a[1], a[2], a[3]);
        } else {
            printf("  [2.%zu] PATCH FAIL: %s\n", i, g_patches[i].desc);
        }
    }

    compile2_fn_t compile2 =
        (compile2_fn_t)dlsym(h, "CompileTfliteFlatbuffer2");
    get_version_fn_t get_ver =
        (get_version_fn_t)dlsym(h, "GetConverterOpFilterVersion");
    if (!compile2 || !get_ver) { printf("dlsym FAIL\n"); return 1; }
    printf("  [3] V2=%p version=%d\n", compile2, get_ver());

    int tflite_fd = open(tflite_path, O_RDONLY);
    if (tflite_fd < 0) { perror("open tflite"); return 1; }
    struct stat st; fstat(tflite_fd, &st);
    int64_t tflite_size = st.st_size;

    int opt_fd = open(options_path, O_RDONLY);
    if (opt_fd < 0) { perror("open opts"); return 1; }
    struct stat ost; fstat(opt_fd, &ost);
    int64_t opt_size = ost.st_size;
    void* opt = malloc(opt_size);
    read(opt_fd, opt, opt_size);
    close(opt_fd);
    printf("  [4] tflite=%lldB opts=%lldB\n",
           (long long)tflite_size, (long long)opt_size);

    {
        FILE* m = fopen("/data/local/tmp/PixelG4A17_marker.txt", "w");
        if (m) { fprintf(m, "PRE_CALL hw=%d\n", hw_soc_rev); fclose(m); }
    }

    int out_fd = -1; int64_t out_bytes = 0; char* out_status = NULL;
    printf("  [5] calling V2(uid=%d)... HW dispatcher will see w0=16 (v6)\n",
           hw_soc_rev);

    int rc = compile2(tflite_fd, tflite_size, opt, opt_size,
                      hw_soc_rev, &out_fd, &out_bytes, &out_status);

    {
        FILE* m = fopen("/data/local/tmp/PixelG4A17_marker.txt", "a");
        if (m) {
            fprintf(m, "POST_CALL rc=%d out_fd=%d out_bytes=%lld\n",
                    rc, out_fd, (long long)out_bytes);
            if (out_status) fprintf(m, "STATUS: %s\n", out_status);
            fclose(m);
        }
    }
    printf("  [6] rc=%d out_fd=%d out_bytes=%lld\n",
           rc, out_fd, (long long)out_bytes);
    if (out_status) printf("       status: %s\n", out_status);

    if (rc == 0 && out_fd >= 0 && out_bytes > 0) {
        void* dgc = mmap(NULL, out_bytes, PROT_READ, MAP_SHARED, out_fd, 0);
        if (dgc != MAP_FAILED) {
            printf("\n  *** DGC PRODUCED — first 64 bytes ***\n  ");
            for (int i = 0; i < 64; i++) printf("%02x ", ((uint8_t*)dgc)[i]);
            printf("\n");
            int wf = open("/data/local/tmp/PixelG4A17.dgc0",
                          O_WRONLY|O_CREAT|O_TRUNC, 0644);
            if (wf >= 0) { write(wf, dgc, out_bytes); close(wf); }
            munmap(dgc, out_bytes);
            printf("  saved %lld B to /data/local/tmp/PixelG4A17.dgc0\n",
                   (long long)out_bytes);
        }
    }

    free(opt); close(tflite_fd);
    if (out_fd >= 0) close(out_fd);
    dlclose(h);
    return rc;
}
