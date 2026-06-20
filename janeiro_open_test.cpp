/**
 * janeiro_open_test.cpp — Diagnostic: does /dev/janeiro open in our context?
 * Does CreateVirtualDevice wire up the IoctlInterface correctly?
 *
 * Build: aarch64-linux-android30-clang++ -std=c++17 -O0 -fPIE -pie \
 *        -static-libstdc++ -target aarch64-linux-android30 \
 *        janeiro_open_test.cpp -o janeiro_open_test -ldl
 * Run:   su -c 'setenforce 0; runcon u:r:hal_camera_default:s0 \
 *               /data/local/tmp/janeiro_open_test'
 */

#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <stdlib.h>

static const char* LIB = "/vendor/lib64/libedgetpu_util.so";

typedef void (*fn_GetDefaultDeviceSpec2)(int*, int8_t*, int*, int*, void**, int*, int8_t*, int8_t*);
typedef void (*fn_CreateVirtualDevice)(int, int8_t, int, int, void*, int, void**);
typedef int  (*fn_GetVersionInfo)(int*);

static long ms_now() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// Scan /proc/self/fd for symlinks containing "janeiro" or "edgetpu"
static void check_self_fds(const char* label) {
    fprintf(stderr, "[test] %s — scanning /proc/self/fd:\n", label);
    DIR* d = opendir("/proc/self/fd");
    if (!d) { fprintf(stderr, "[test]   opendir failed: %s\n", strerror(errno)); return; }
    int found = 0;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        char path[256], target[256];
        snprintf(path, sizeof(path), "/proc/self/fd/%s", ent->d_name);
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = 0;
        if (strstr(target, "janeiro") || strstr(target, "edgetpu")) {
            fprintf(stderr, "[test]   fd %s -> %s\n", ent->d_name, target);
            found++;
        }
    }
    closedir(d);
    if (!found) fprintf(stderr, "[test]   (no january/edgetpu fds found)\n");
}

// Snapshot ALL /proc/self/fd into a bitmask (fd <= 1023)
static uint64_t fd_snapshot[16] = {};  // 16 x 64 bits = 1024 fds

static void snapshot_fds(uint64_t* snap) {
    memset(snap, 0, 16 * sizeof(uint64_t));
    DIR* d = opendir("/proc/self/fd");
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        int fd = atoi(ent->d_name);
        if (fd >= 0 && fd < 1024) snap[fd / 64] |= (1ULL << (fd % 64));
    }
    closedir(d);
}

static void print_new_fds(const uint64_t* before, const uint64_t* after) {
    for (int i = 0; i < 1024; i++) {
        bool was = (before[i/64] >> (i%64)) & 1;
        bool now  = (after [i/64] >> (i%64)) & 1;
        if (!was && now) {
            char path[256], target[256];
            snprintf(path, sizeof(path), "/proc/self/fd/%d", i);
            ssize_t n = readlink(path, target, sizeof(target) - 1);
            if (n > 0) { target[n] = 0; fprintf(stderr, "[test]   NEW fd %d -> %s\n", i, target); }
            else        fprintf(stderr, "[test]   NEW fd %d (unresolvable)\n", i);
        }
    }
}

// Print 64 bytes at ptr as hex, 16 per row
static void hexdump(const char* label, const void* ptr, size_t len) {
    fprintf(stderr, "[test] %s @ %p:\n", label, ptr);
    const uint8_t* p = (const uint8_t*)ptr;
    for (size_t i = 0; i < len; i += 16) {
        fprintf(stderr, "  %04zx: ", i);
        for (size_t j = i; j < i + 16 && j < len; j++)
            fprintf(stderr, "%02x ", p[j]);
        fprintf(stderr, "\n");
    }
}

// Check if an address looks like it's in a shared library's .text section
// (rough heuristic: not heap, not stack, not null)
static void classify_ptr(const char* label, uintptr_t addr) {
    if (addr == 0)        { fprintf(stderr, "[test] %s: 0x0 — NULL\n", label); return; }
    if (addr < 0x10000)   { fprintf(stderr, "[test] %s: 0x%lx — tiny value (probably not a ptr)\n", label, addr); return; }

    // Try to find in /proc/self/maps
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f)               { fprintf(stderr, "[test] %s: 0x%lx — (can't read maps)\n", label, addr); return; }

    char line[512];
    uintptr_t base, end;
    char perms[8], rest[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%lx-%lx %s %*s %*s %*s %255[^\n]", &base, &end, perms, rest) < 3) continue;
        // Strip MTE tag for comparison
        uintptr_t check = addr & 0x00ffffffffffffffULL;
        if (check >= base && check < end) {
            fprintf(stderr, "[test] %s: 0x%lx → %s %s\n", label, addr, perms, rest[0] ? rest : "(anon)");
            fclose(f);
            return;
        }
    }
    fclose(f);
    fprintf(stderr, "[test] %s: 0x%lx — not found in maps\n", label, addr);
}

int main() {
    fprintf(stderr, "[test] janeiro_open_test — PID=%d\n", (int)getpid());

    // ── 1. Direct open of /dev/janeiro ──────────────────────────────────────
    fprintf(stderr, "\n[test] === 1. Direct open /dev/janeiro ===\n");
    int direct_fd = open("/dev/janeiro", O_RDWR);
    if (direct_fd < 0) {
        fprintf(stderr, "[test] open FAILED: %s (errno=%d)\n", strerror(errno), errno);
    } else {
        fprintf(stderr, "[test] open OK: fd=%d\n", direct_fd);
        close(direct_fd);
    }

    // ── 2. Before dlopen, check our FDs ──────────────────────────────────────
    check_self_fds("before dlopen");

    // ── 3. Load library and call GetDefaultDeviceSpec2 + CreateVirtualDevice ──
    fprintf(stderr, "\n[test] === 2. dlopen + GetDefaultDeviceSpec2 + CreateVirtualDevice ===\n");

    void* lib = dlopen(LIB, RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { fprintf(stderr, "[test] dlopen failed: %s\n", dlerror()); return 1; }
    fprintf(stderr, "[test] dlopen OK\n");

    auto GetVersionInfo      = (fn_GetVersionInfo)     dlsym(lib, "DarwinnDelegate_GetVersionInfo");
    auto GetDefaultDeviceSpec2 = (fn_GetDefaultDeviceSpec2) dlsym(lib, "DarwinnDelegate_GetDefaultDeviceSpec2");
    auto CreateVirtualDevice = (fn_CreateVirtualDevice) dlsym(lib, "DarwinnDelegate_CreateVirtualDevice");

    if (!GetVersionInfo || !GetDefaultDeviceSpec2 || !CreateVirtualDevice) {
        fprintf(stderr, "[test] missing symbols\n"); return 1;
    }

    int version = 0;
    GetVersionInfo(&version);
    fprintf(stderr, "[test] GetVersionInfo: version=%d\n", version);

    int type = 0, p2 = 0, p3 = 0, p5 = 0;
    int8_t chip = 0, p6 = 0, p7 = 0;
    void* cfg = nullptr;
    GetDefaultDeviceSpec2(&type, &chip, &p2, &p3, &cfg, &p5, &p6, &p7);
    fprintf(stderr, "[test] DeviceSpec2: type=%d chip=%d p2=%d p3=%d cfg=%p p5=%d\n",
            type, (int)chip, p2, p3, cfg, p5);

    // Check FDs after GetDefaultDeviceSpec2
    check_self_fds("after GetDefaultDeviceSpec2");

    void* vdev = nullptr;
    CreateVirtualDevice(type, chip, p2, p3, cfg, p5, &vdev);
    fprintf(stderr, "[test] VirtualDevice @ %p\n", vdev);

    // ── 4. Check FDs after CreateVirtualDevice ────────────────────────────────
    check_self_fds("after CreateVirtualDevice");

    if (!vdev) { fprintf(stderr, "[test] VirtualDevice null — exiting\n"); return 1; }

    // ── 5. Poll vdev[+0x48] for 6 s (100 ms intervals) ──────────────────────
    fprintf(stderr, "\n[test] === 3. Polling vdev[+0x48] (60 x 100ms = 6s) ===\n");
    for (int i = 0; i < 60; i++) {
        usleep(100000);
        void* slot = *(void**)((char*)vdev + 0x48);
        fprintf(stderr, "[%d00ms] vdev[+0x48] = %p\n", i + 1, slot);
        if (slot != nullptr) { fprintf(stderr, "POPULATED\n"); break; }
    }

    fprintf(stderr, "\n[test] done.\n");
    return 0;
}
