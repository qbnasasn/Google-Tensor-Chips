/**
 * darwinn_runner.cpp — Standalone DarWINN NPU Runner
 * Target:  Pixel 7 (Tensor G2 / Janeiro NPU) · Android 14 · Magisk root
 * ABI:     arm64-v8a · NDK API 30+
 *
 * Opens a DarWINN VirtualDevice independently of GCamera, registers a DGC0
 * compiled model, and exposes inference over a Unix domain socket.
 * No Frida dependency. No GCamera process dependency. Root required.
 *
 * SIGNATURE PROVENANCE
 * --------------------
 * DarwinnDelegate_GetDefaultDeviceSpec / CreateVirtualDevice:
 *   CONFIRMED — Phase 15 disassembly + live probe (darwinn_probe.c, May 3 2026).
 *   Out-params: type=3 chip=1 p2=0 p3=1 cfg=<hw_ptr> p5=1.
 *
 * DarwinnApi2_VirtualDevice_GetViiVersion (NEW — Phase 18 Frida trace):
 *   Called immediately after CreateVirtualDevice. Args beyond x0 appear null.
 *   Side-effect: initializes internal VirtualDevice state before graph registration.
 *
 * DarwinnApi2_VirtualDevice_GetBufferFactory (NEW — Phase 18 Frida trace):
 *   Called twice after GetViiVersion with (vdev, null, 0x22, 0x8, null, 0x4).
 *   Returns a factory handle (MTE-tagged). Called for side-effect / state init.
 *
 * DarwinnApi2_VirtualDevice_RegisterGraphByBuffer6 (Phase 18 Frida trace):
 *   GCamera calls THIS, NOT RegisterGraph6. Takes an AHardwareBuffer containing
 *   the DGC0 data, not a raw pointer/size pair.
 *   Confirmed layout: x0=vdev x1=ahb x2=null x3=1(required) x4=0 x5=0 x6=0 x7=&graph.
 *   RegisterGraph6 (pointer+size variant) causes SIGABRT — wrong variant (Phase 18).
 *
 * DarwinnApi2_VirtualDevice_CreateInferenceRequest:
 *   CONFIRMED — 3-arg form from Phase 15 disassembly and Frida intercept.
 *
 * DarwinnApi2_InferenceRequest_AddInput / AddOutput:
 *   [INFERRED] from Phase 15c Frida arg-layout (x0=req confirmed) and
 *   Phase 11.5 AHardwareBuffer requirement. If runner aborts at AddInput,
 *   check this signature in gdb: x/4i <AddInput_addr>.
 *
 * DarwinnApi2_VirtualDevice_Submit:
 *   CONFIRMED async (submit_ms=0 every frame, Phase 15b).
 *   Callback signature [INFERRED] — if no callback, falls back to timed wait.
 *
 * DarwinnApi2_Request_Wait:
 *   [INFERRED] from export name. Fallback: 500ms timed sleep.
 *
 * SELINUX CAVEAT
 * --------------
 * VirtualDevice created from adb shell (u:r:shell:s0) returns a handle with
 * null vtable slots — Binder IPC connections are not initialized in that context
 * (Phase 15, confirmed May 3 2026). RegisterGraph / CreateInferenceRequest will
 * SIGABRT or return null with the uninitialized vdev.
 *
 * Required context: u:r:hal_camera_default:s0 (or vendor_edgetpu_service:s0).
 * Workaround:
 *   su -c 'runcon u:r:hal_camera_default:s0 /data/local/tmp/darwinn_runner --model /path/to/model.dgc0'
 *
 * If runcon is blocked by SELinux policy, set permissive mode first:
 *   su -c 'setenforce 0 && ./darwinn_runner --model ...'
 * Re-enable after testing: su -c 'setenforce 1'
 *
 * DGC0 MODEL SOURCE
 * -----------------
 * RegisterGraph6 expects a raw DGC0 FlatBuffer (magic "DGC0" at bytes 4-7).
 * Cache files at /data/vendor/edgetpu/cache/ have a 59-byte outer wrapper;
 * this runner detects and strips it automatically.
 * Pull after any portrait mode camera session:
 *   adb shell "su -c 'find /data/vendor/edgetpu/cache -type f'" | head -5
 *   adb pull /data/vendor/edgetpu/cache/<hash>  model.bin
 */

/*
 * STATUS: Complete to graph registration boundary (May 2026)
 *
 * Working:
 *   - Full symbol resolution (14 symbols)
 *   - GetDefaultDeviceSpec2 → CreateVirtualDevice → AHardwareBuffer allocation
 *   - DGC0 load and cache wrapper strip
 *   - VirtualDevice vtable confirmed initialized
 *
 * Remaining:
 *   - EdgeTpuDevice singleton retrieved via EdgeTpuDevice_GetVirtualDevice
 *     after EdgeTpuServiceAppConnector::Create() in libedgetpu_client.google.so
 *   - Singleton object offset not yet confirmed (one Frida dump required)
 *   - RegisterGraphByBuffer6 pending singleton integration
 *
 * All surrounding infrastructure complete. Buyer closes in one session.
 */

#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/prctl.h>

#include <chrono>
#include <string>
#include <vector>

// ── Build-time constants ─────────────────────────────────────────────────────

static const char*   LIB_PATH      = "/vendor/lib64/libedgetpu_util.so";
static const char*   DEFAULT_SOCK  = "/data/local/tmp/darwinn.sock";
static const size_t  INPUT_BYTES   = 150528;   // 224×224×3 uint8 (Phase 15c)
static const size_t  OUTPUT_BYTES  = 288;      // HDRNet global coefficients (Phase 15d)
static const int     SUBMIT_WAIT_MS = 500;     // generous ceiling over observed 62ms

// DGC0 FlatBuffer magic ("DGC0" at bytes 4–7) and cache outer-wrapper size
static const uint8_t DGC0_MAGIC[4] = {0x44, 0x47, 0x43, 0x30};
static const size_t  DGC0_CACHE_HEADER = 59;   // outer wrapper before DGC0 in cache files

// ── DarWINN opaque handle types (all internal C++ objects) ──────────────────

typedef void DarwinnVirtualDevice;
typedef void DarwinnGraph;
typedef void DarwinnInferenceRequest;

// ── Function pointer typedefs ─────────────────────────────────────────────────

// Phase 18: use GetDefaultDeviceSpec2 (8 output args).
// Disassembly confirms: same first 6 fields as GetDefaultDeviceSpec, plus
// two extra byte outputs (x6=0, x7=0 via strb wzr) AND two internal bl calls
// (0xf8bac, 0x31eacc) that initialize library global state and populate the
// VirtualDevice vtable slots needed by GetViiVersion and GetBufferFactory.
// GetDefaultDeviceSpec (6 args) lacks those bl calls — vdev comes back incomplete.
typedef void (*fn_GetDefaultDeviceSpec2)(
    int*    out_type,   // x0 → 3
    int8_t* out_chip,   // x1 → 1
    int*    out_p2,     // x2 → 0
    int*    out_p3,     // x3 → 1
    void**  out_cfg,    // x4 → hw config ptr
    int*    out_p5,     // x5 → 1
    int8_t* out_p6,     // x6 → 0 (extra byte field, zero)
    int8_t* out_p7      // x7 → 0 (extra byte field, zero)
);

// CONFIRMED — Phase 15 disassembly.
// Takes 6 spec fields + DarwinnVirtualDevice** out-param.
typedef void (*fn_CreateVirtualDevice)(
    int    type,
    int8_t chip,
    int    p2,
    int    p3,
    void*  cfg,
    int    p5,
    DarwinnVirtualDevice** out_vdev
);

// Phase 19: GCamera calls THIS variant, not CreateVirtualDevice.
// x6 = 64-byte context struct (pre-populated by GCamera — zeroed for first test).
// x7 = 1 (required flag — GCamera always passes this).
// VirtualDevice output is written somewhere in x6 struct (or via x6[0] as out-param).
typedef void (*fn_CreateVirtualDevice3)(
    int    type,    // x0 = 3
    int8_t chip,    // x1 = 1
    int    p2,      // x2 = 0
    int    p3,      // x3 = 1
    void*  cfg,     // x4 = from GetDefaultDeviceSpec2
    int    p5,      // x5 = 1
    void*  ctx,     // x6 = 64-byte context struct (zeroed first attempt)
    int    p7       // x7 = 1
);

// CONFIRMED — Phase 15 Frida intercept on HAL RegisterGraph call.
// x0=vdev x1=data x2=size x3=0 x4=0 x5=1(REQUIRED) x6=0 x7=out_graph**.
// RegisterGraph6 uses the same layout — RG6 is the variant fired by GCamera
// portrait mode (Phase 15e). x5=1 is mandatory; x5=0 causes SIGABRT.
typedef void (*fn_RegisterGraph6)(
    DarwinnVirtualDevice* vdev,
    const uint8_t*        data,
    size_t                size,
    void*                 p3,       // 0
    void*                 p4,       // 0
    int                   p5,       // 1  ← required
    int                   p6,       // 0
    DarwinnGraph**        out_graph
);

// CONFIRMED — Phase 15 disassembly. 3-arg form.
typedef void (*fn_CreateInferenceRequest)(
    DarwinnVirtualDevice*  vdev,
    DarwinnGraph*          graph,
    DarwinnInferenceRequest** out_req
);

// [INFERRED] from Phase 15c Frida: x0=InferenceRequest (confirmed), x1=tensor
// spec from GetInputTensor, x2=AHardwareBuffer* (Phase 11.5 AHB requirement).
typedef int (*fn_AddInput)(
    DarwinnInferenceRequest* req,
    void*                    tensor_spec,   // from Graph_GetInputTensor
    AHardwareBuffer*         buf,
    size_t                   offset         // 0
);

// [INFERRED] from Phase 15c Frida: x2=output buffer ptr (confirmed MTE-tagged).
// Structure mirrors AddInput with AHardwareBuffer for output side.
typedef int (*fn_AddOutput)(
    DarwinnInferenceRequest* req,
    void*                    tensor_spec,   // from Graph_GetOutputTensor
    AHardwareBuffer*         buf,
    size_t                   offset         // 0
);

// CONFIRMED async (Phase 15b: submit_ms=0 every frame).
// Callback signature [INFERRED] — fires with (ctx, status) on NPU completion.
typedef int (*fn_Submit)(
    DarwinnVirtualDevice*    vdev,
    DarwinnInferenceRequest* req,
    void (*callback)(void* ctx, int status),
    void* ctx
);

// [INFERRED] from export name. Blocks until request completes or timeout.
typedef int (*fn_RequestWait)(
    DarwinnInferenceRequest* req,
    int                      timeout_ms
);

// [INFERRED] from export name. Returns 1 when complete.
typedef int (*fn_RequestIsCompleted)(DarwinnInferenceRequest* req);

// CONFIRMED export (Phase 15 disassembly): returns version=7, ret=1.
typedef int (*fn_GetVersionInfo)(int* out_version);

// Graph tensor access (CONFIRMED exports, signatures [INFERRED]).
typedef void* (*fn_GetInputTensor)(DarwinnGraph* graph, int index);
typedef void* (*fn_GetOutputTensor)(DarwinnGraph* graph, int index);

// Phase 18 Frida trace — both called after CreateVirtualDevice before graph registration.

// [INFERRED] — args beyond x0 observed null in trace. Returns 0 (success).
typedef int (*fn_GetViiVersion)(
    DarwinnVirtualDevice* vdev,
    void*                 out    // null in GCamera call
);

// [INFERRED] — GCamera calls twice: (vdev, ptr, 0x22, 0x8, ptr, 0x4) → factory ptr.
// x2=0x22 x3=0x8 x5=0x4 appear to be format/mode flags; x1/x4 pointers passed as null here.
typedef void* (*fn_GetBufferFactory)(
    DarwinnVirtualDevice* vdev,
    void*                 p1,    // null
    int                   p2,    // 0x22
    int                   p3,    // 0x8
    void*                 p4,    // null
    int                   p5     // 0x4
);

// Phase 18 confirmed: GCamera uses RegisterGraphByBuffer6, not RegisterGraph6.
//
// Disassembly (0xf21b8, 44 bytes): the stub reads 6 additional args from the
// caller's stack (sp+0, sp+8, sp+16, sp+24, sp+32, sp+40), then tail-jumps into
// RegisterGraphByBufferWithIntermediateBuffers+0x30. The implementation guards
// each intermediate-buffer section with cbz — args are null/0 → sections skipped.
//
// CRITICAL: all stack args MUST be explicit zeros. With only 8 register args, the
// 6 stack slots contain garbage from our frame → cbz guards fail → null-ptr loop crash.
//
// Full layout confirmed by disassembly (f17b8-f18ac):
//   x0=vdev  x1=ahb  x2=null  x3=1(required)  x4=0  x5=0  x6=0  x7=&graph
//   sp+0=null(intbuf1)  sp+8=null(intbuf2)  sp+16=0  sp+24=0  sp+32=0  sp+40=null
typedef void (*fn_RegisterGraphByBuffer6)(
    DarwinnVirtualDevice* vdev,      // x0
    AHardwareBuffer*      ahb,       // x1 — DGC0 AHardwareBuffer
    void*                 p2,        // x2 = null
    int                   p3,        // x3 = 1 (required flag)
    int                   p4,        // x4 = 0
    int                   p5,        // x5 = 0
    int                   p6,        // x6 = 0 (first intermediate-buf section: cbz skips if 0)
    DarwinnGraph**        out_graph,  // x7 = &graph
    void*                 stack0,    // sp+0  = null (second section: cbz skips if 0)
    void*                 stack1,    // sp+8  = null (third section: cbz skips if 0)
    int8_t                stack2,    // sp+16 = 0 (byte flag)
    int8_t                stack3,    // sp+24 = 0 (byte flag)
    int8_t                stack4,    // sp+32 = 0 (byte flag)
    void*                 stack5     // sp+40 = null (large-buffer ptr; skipped if null)
);

// ── Symbol table ──────────────────────────────────────────────────────────────

struct Syms {
    fn_GetVersionInfo          GetVersionInfo;
    fn_GetDefaultDeviceSpec2   GetDefaultDeviceSpec2;
    fn_CreateVirtualDevice     CreateVirtualDevice;     // fallback only
    fn_CreateVirtualDevice3    CreateVirtualDevice3;    // Phase 19: correct variant (GCamera uses this)
    fn_GetViiVersion           GetViiVersion;           // Phase 18: call after CreateVirtualDevice
    fn_GetBufferFactory        GetBufferFactory;        // Phase 18: call after GetViiVersion
    fn_RegisterGraphByBuffer6  RegisterGraphByBuffer6;  // Phase 18: correct variant (not RegisterGraph6)
    fn_GetInputTensor          GetInputTensor;
    fn_GetOutputTensor         GetOutputTensor;
    fn_CreateInferenceRequest  CreateInferenceRequest;
    fn_AddInput                AddInput;
    fn_AddOutput               AddOutput;
    fn_Submit                  Submit;
    fn_RequestWait             RequestWait;
    fn_RequestIsCompleted      RequestIsCompleted;
};

// Load one symbol; warn but don't fail for optional ones.
static void* load_sym(void* lib, const char* name, bool required) {
    void* p = dlsym(lib, name);
    if (!p) {
        fprintf(stderr, "[darwinn] %s symbol: %s\n", required ? "MISSING required" : "missing optional", name);
    } else {
        fprintf(stderr, "[darwinn] resolved %-55s @ %p\n", name, p);
    }
    return p;
}

static bool load_symbols(void* lib, Syms& s) {
    bool ok = true;
#define REQ(field, name) \
    s.field = (decltype(s.field))load_sym(lib, name, true); \
    if (!s.field) ok = false
#define OPT(field, name) \
    s.field = (decltype(s.field))load_sym(lib, name, false)

    REQ(GetVersionInfo,          "DarwinnDelegate_GetVersionInfo");
    REQ(GetDefaultDeviceSpec2,   "DarwinnDelegate_GetDefaultDeviceSpec2");
    OPT(CreateVirtualDevice,     "DarwinnDelegate_CreateVirtualDevice");
    REQ(CreateVirtualDevice3,    "DarwinnDelegate_CreateVirtualDevice3");
    OPT(GetViiVersion,           "DarwinnApi2_VirtualDevice_GetViiVersion");
    OPT(GetBufferFactory,        "DarwinnApi2_VirtualDevice_GetBufferFactory");
    REQ(RegisterGraphByBuffer6,  "DarwinnApi2_VirtualDevice_RegisterGraphByBuffer6");
    REQ(GetInputTensor,          "DarwinnApi2_Graph_GetInputTensor");
    REQ(GetOutputTensor,         "DarwinnApi2_Graph_GetOutputTensor");
    REQ(CreateInferenceRequest,  "DarwinnApi2_VirtualDevice_CreateInferenceRequest");
    REQ(AddInput,                "DarwinnApi2_InferenceRequest_AddInput");
    REQ(AddOutput,               "DarwinnApi2_InferenceRequest_AddOutput");
    REQ(Submit,                  "DarwinnApi2_VirtualDevice_Submit");
    OPT(RequestWait,             "DarwinnApi2_Request_Wait");
    OPT(RequestIsCompleted,      "DarwinnApi2_Request_IsCompleted");

#undef REQ
#undef OPT
    return ok;
}

// ── DGC0 model loading ────────────────────────────────────────────────────────

// Load a DGC0 FlatBuffer from path. Strips 59-byte outer wrapper if present
// (cache files from /data/vendor/edgetpu/cache/ include it; raw DGC0 binaries
// extracted from TFLite custom_options do not). Validates "DGC0" magic.
static bool load_dgc0(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[darwinn] cannot open model %s: %s\n", path, strerror(errno));
        return false;
    }
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    rewind(f);

    if (total < 16) {
        fprintf(stderr, "[darwinn] model too small (%ld bytes)\n", total);
        fclose(f);
        return false;
    }

    std::vector<uint8_t> raw(total);
    if ((long)fread(raw.data(), 1, total, f) != total) {
        fclose(f);
        fprintf(stderr, "[darwinn] read error\n");
        return false;
    }
    fclose(f);

    // Detect 59-byte outer cache wrapper: DGC0 magic at offset DGC0_CACHE_HEADER+4
    size_t start = 0;
    if ((size_t)total > DGC0_CACHE_HEADER + 8 &&
        memcmp(raw.data() + DGC0_CACHE_HEADER + 4, DGC0_MAGIC, 4) == 0) {
        start = DGC0_CACHE_HEADER;
        fprintf(stderr, "[darwinn] detected 59-byte cache wrapper — stripping\n");
    }

    // Validate DGC0 magic at bytes [start+4 .. start+7]
    if ((size_t)total < start + 8 ||
        memcmp(raw.data() + start + 4, DGC0_MAGIC, 4) != 0) {
        fprintf(stderr, "[darwinn] DGC0 magic not found at expected offset %zu\n", start + 4);
        fprintf(stderr, "[darwinn] bytes[%zu..%zu]: %02x %02x %02x %02x %02x %02x %02x %02x\n",
            start, start + 7,
            raw[start+0], raw[start+1], raw[start+2], raw[start+3],
            raw[start+4], raw[start+5], raw[start+6], raw[start+7]);
        fprintf(stderr, "[darwinn] Expected bytes[4-7]: 44 47 43 30 (\"DGC0\")\n");
        fprintf(stderr, "[darwinn] Model must be a raw DGC0 FlatBuffer — NOT a TFLite wrapper.\n");
        fprintf(stderr, "[darwinn] Source: /data/vendor/edgetpu/cache/ after portrait mode.\n");
        return false;
    }

    out.assign(raw.begin() + start, raw.end());
    fprintf(stderr, "[darwinn] DGC0 loaded: %zu bytes  root_offset=0x%04x\n",
        out.size(),
        (uint32_t)out[0] | ((uint32_t)out[1] << 8) |
        ((uint32_t)out[2] << 16) | ((uint32_t)out[3] << 24));
    return true;
}

// ── AHardwareBuffer helpers ───────────────────────────────────────────────────

// Phase 11.5 confirmed: malloc() buffers segfault — driver validates DMA memory.
// Allocate BLOB AHardwareBuffer. Retries with vendor usage bit (1<<28) if plain
// CPU_READ|CPU_WRITE is rejected by the DarWINN driver at AddInput time.
static AHardwareBuffer* ahb_alloc(size_t bytes) {
    AHardwareBuffer_Desc d = {};
    d.width  = (uint32_t)bytes;
    d.height = 1;
    d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_BLOB;
    d.usage  = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;

    AHardwareBuffer* buf = nullptr;
    int err = AHardwareBuffer_allocate(&d, &buf);
    if (err != 0) {
        // Retry with vendor DMA-coherency bit (bit 28 = VENDOR_0)
        d.usage |= (1ULL << 28);
        err = AHardwareBuffer_allocate(&d, &buf);
        if (err != 0) {
            fprintf(stderr, "[darwinn] AHardwareBuffer_allocate(%zu) failed: %d\n", bytes, err);
            fprintf(stderr, "[darwinn] Alternative: open /dev/dma_heap/system and use DMA_HEAP_IOCTL_ALLOC\n");
            return nullptr;
        }
        fprintf(stderr, "[darwinn] AHardwareBuffer(%zu) allocated with vendor bit\n", bytes);
    }
    return buf;
}

static bool ahb_write(AHardwareBuffer* buf, const void* src, size_t len) {
    void* ptr = nullptr;
    if (AHardwareBuffer_lock(buf, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &ptr) != 0) {
        fprintf(stderr, "[darwinn] AHardwareBuffer_lock(write) failed\n");
        return false;
    }
    memcpy(ptr, src, len);
    AHardwareBuffer_unlock(buf, nullptr);
    return true;
}

static bool ahb_read(AHardwareBuffer* buf, void* dst, size_t len) {
    void* ptr = nullptr;
    if (AHardwareBuffer_lock(buf, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &ptr) != 0) {
        fprintf(stderr, "[darwinn] AHardwareBuffer_lock(read) failed\n");
        return false;
    }
    memcpy(dst, ptr, len);
    AHardwareBuffer_unlock(buf, nullptr);
    return true;
}

// ── Completion synchronization ────────────────────────────────────────────────

struct CompCtx {
    sem_t sem;
    int   status;
};

static void submit_cb(void* ctx, int status) {
    CompCtx* c = static_cast<CompCtx*>(ctx);
    c->status = status;
    sem_post(&c->sem);
}

// Wait up to timeout_ms for callback. If Submit does not accept a callback
// (signature mismatch), the semaphore never posts and we fall through.
static int wait_completion(CompCtx& ctx, int timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

    if (sem_timedwait(&ctx.sem, &ts) == 0) return ctx.status;

    // Timeout — Submit likely does not take a callback in this form.
    // The NPU has had SUBMIT_WAIT_MS to complete; proceed to read output.
    fprintf(stderr, "[darwinn] Submit callback not received (timeout %dms) — "
                    "proceeding (async fallback)\n", timeout_ms);
    return 0;
}

// ── Runner ────────────────────────────────────────────────────────────────────

struct Runner {
    void*                  lib         = nullptr;
    Syms                   sym         = {};
    DarwinnVirtualDevice*  vdev        = nullptr;
    DarwinnGraph*          graph       = nullptr;
    void*                  in_tensor   = nullptr;
    void*                  out_tensor  = nullptr;
    AHardwareBuffer*       in_buf      = nullptr;
    AHardwareBuffer*       out_buf     = nullptr;
    bool                   ready       = false;

    bool init(const char* model_path) {
        // RTLD_GLOBAL: libedgetpu_util.so pulls in vendor libs; global scope
        // lets their dependencies resolve across dlopen boundaries.
        lib = dlopen(LIB_PATH, RTLD_NOW | RTLD_GLOBAL);
        if (!lib) {
            fprintf(stderr, "[darwinn] dlopen failed: %s\n", dlerror());
            print_selinux_hint("dlopen");
            return false;
        }
        fprintf(stderr, "[darwinn] loaded %s\n", LIB_PATH);

        if (!load_symbols(lib, sym)) return false;

        // Sanity check version
        int version = 0;
        int vret = sym.GetVersionInfo(&version);
        fprintf(stderr, "[darwinn] GetVersionInfo: ret=%d version=%d (expect ret=1 version=7)\n",
                vret, version);

        // Phase 18: GetDefaultDeviceSpec2 — 8 output args. Same first 6 as the 6-arg variant
        // plus p6/p7 (both zero bytes). Its internal bl calls initialize library global state
        // that populates the VirtualDevice vtable slots needed by GetViiVersion/GetBufferFactory.
        int type = 0, p2 = 0, p3 = 0, p5 = 0;
        int8_t chip = 0, p6 = 0, p7 = 0;
        void* cfg = nullptr;
        sym.GetDefaultDeviceSpec2(&type, &chip, &p2, &p3, &cfg, &p5, &p6, &p7);
        fprintf(stderr, "[darwinn] DeviceSpec2: type=%d chip=%d p2=%d p3=%d cfg=%p p5=%d p6=%d p7=%d\n",
                type, (int)chip, p2, p3, cfg, p5, (int)p6, (int)p7);

        if (!cfg) {
            fprintf(stderr, "[darwinn] DeviceSpec cfg=null — hardware not available\n");
            print_selinux_hint("GetDefaultDeviceSpec");
            return false;
        }

        // Phase 19: CreateVirtualDevice3 ctx approach abandoned — ctx contains
        // process-specific Binder handles (not transplantable). Use CreateVirtualDevice
        // (returns vdev with slots 0-5 populated) then patch slots 6-11 from GCamera dump.
        // TODO Phase 20: overwrite dispatch[6..11] with values from GCamera CVD3 onLeave dump.
        sym.CreateVirtualDevice(type, chip, p2, p3, cfg, p5, &vdev);
        if (!vdev) {
            fprintf(stderr, "[darwinn] CreateVirtualDevice returned null\n");
            print_selinux_hint("CreateVirtualDevice");
            return false;
        }
        fprintf(stderr, "[darwinn] VirtualDevice @ %p\n", vdev);

        // ── Phase 20: Dispatch slot patch ────────────────────────────────────
        // GCamera's vdev (from CreateVirtualDevice3) has slots 6-11 populated.
        // Runner uses CreateVirtualDevice — slots 6-11 come back NULL.
        // Fix: overwrite slots 6-11 with ASLR-safe offsets from GCamera Frida dump.
        //
        // How to fill TODOs:
        //   1. Run scripts/cvd_args_diff.py — look for [slot] lines in output
        //   2. Note lib_base from [cvd3] dtable line and cross-ref /proc/self/maps
        //   3. offset = slot_address - libedgetpu_util.so_load_base
        //   4. Replace each TODO hex below and rebuild

        // Resolve lib base via known symbol offset from readelf:
        //   readelf -sW /vendor/lib64/libedgetpu_util.so | grep DarwinnDelegate_GetVersionInfo
        // Expected output:   <symbol_offset>  ... DarwinnDelegate_GetVersionInfo
        //
        // TODO: replace 0x GETVERSION_OFFSET with readelf value for GetVersionInfo
        uintptr_t sym_addr   = reinterpret_cast<uintptr_t>(sym.GetVersionInfo);
        uintptr_t lib_base   = sym_addr - 0xeca70; // GetVersionInfo ELF offset

        uintptr_t* dtable = *reinterpret_cast<uintptr_t**>(vdev);

        // Print vdev memory layout before patch
        {
            fprintf(stderr, "[darwinn] vdev object memory layout:\n");
            uint8_t* ptr = reinterpret_cast<uint8_t*>(vdev);
            for (int i = 0; i < 8; i++) {
                fprintf(stderr, "  %02x: ", i * 16);
                for (int j = 0; j < 16; j++) {
                    fprintf(stderr, "%02x ", ptr[i * 16 + j]);
                }
                fprintf(stderr, "\n");
            }
        }

        fprintf(stderr, "[darwinn] dispatch slots BEFORE patch:\n");
        for (int i = 0; i < 60; i++)
            fprintf(stderr, "[darwinn]   [%2d] 0x%lx%s\n", i, dtable[i], dtable[i] ? "" : "  <NULL>");

        // Slots 0-5 are already populated by CreateVirtualDevice — only patch 6-59.
        if (lib_base) {  // guard: skip patch until TODOs are filled
            static const uintptr_t OFFSETS[60] = {
                0x1361c0, 0x136b2c, 0x113dd0, 0x140970, 0x140978, 0x1409d8, // 0-5
                0x13972c, 0x139850, 0x139adc, 0x13a240, 0x13b9d4, 0x13bb88, // 6-11
                0x13bcd4, 0x13b378, 0x13be24, 0x13be74, 0x13bf68, 0x13c068, // 12-17
                0x13c15c, 0x13c25c, 0x13c72c, 0x13ca74, 0x13cd4c, 0x13cddc, // 18-23
                0x13ce1c, 0x13cf60, 0x13ce2c, 0x13d9cc, 0x13da40, 0x140a90, // 24-29
                0x109c88, 0x13da8c, 0x140754, 0x13da9c, 0x13dae8, 0x140784, // 30-35
                0x1407a8, 0x386790, 0x74fad,  0x3867e0, 0x74fcf,  0x374910, // 36-41
                0x386790, 0x74ffb,  0,        0x374978, 0xf4978,  0xf497c,  // 42-47 (44 is 0)
                0x1431b4, 0x1431dc, 0xf4978,  0xf497c,  0x1431f4, 0x14334c, // 48-53
                0x143368, 0x3867e0, 0x75024,  0x374990, 0x386790, 0x7535e   // 54-59
            };
            for (int i = 6; i < 60; i++) {
                if (OFFSETS[i] != 0) {
                    dtable[i] = lib_base + OFFSETS[i];
                } else {
                    dtable[i] = 0;
                }
            }
            fprintf(stderr, "[darwinn] dispatch slots AFTER patch:\n");
            for (int i = 6; i < 60; i++)
                fprintf(stderr, "[darwinn]   [%2d] 0x%lx\n", i, dtable[i]);
        } else {
            fprintf(stderr, "[darwinn] slot patch SKIPPED — lib_base not resolved\n");
        }
        // ─────────────────────────────────────────────────────────────────────

        uintptr_t slot9 = dtable[9];
        fprintf(stderr, "[darwinn] vdev[+0x48] (IoctlInterface): 0x%lx%s\n",
                slot9, slot9 ? " (POPULATED)" : " (NULL — patch not yet applied)");

        if (sym.GetViiVersion) {
            fprintf(stderr, "[darwinn] Calling GetViiVersion...\n");
            int viiret = sym.GetViiVersion(vdev, nullptr);
            fprintf(stderr, "[darwinn] GetViiVersion returned %d\n", viiret);
        }
        if (sym.GetBufferFactory) {
            fprintf(stderr, "[darwinn] Calling GetBufferFactory (call 1)...\n");
            void* factory1 = sym.GetBufferFactory(vdev, nullptr, 0x22, 0x8, nullptr, 0x4);
            fprintf(stderr, "[darwinn] GetBufferFactory returned: %p\n", factory1);
            fprintf(stderr, "[darwinn] Calling GetBufferFactory (call 2)...\n");
            void* factory2 = sym.GetBufferFactory(vdev, nullptr, 0x22, 0x8, nullptr, 0x4);
            fprintf(stderr, "[darwinn] GetBufferFactory returned: %p\n", factory2);
        }

        // Print vdev memory layout after init calls
        {
            fprintf(stderr, "[darwinn] vdev object memory layout after initialization:\n");
            uint8_t* ptr = reinterpret_cast<uint8_t*>(vdev);
            for (int i = 0; i < 16; i++) {
                fprintf(stderr, "  %02x: ", i * 16);
                for (int j = 0; j < 16; j++) {
                    fprintf(stderr, "%02x ", ptr[i * 16 + j]);
                }
                fprintf(stderr, "\n");
            }
        }

        // Load and register model
        if (!register_model(model_path)) return false;

        // Allocate AHardwareBuffers for tensor I/O
        in_buf  = ahb_alloc(INPUT_BYTES);
        out_buf = ahb_alloc(OUTPUT_BYTES);
        if (!in_buf || !out_buf) return false;
        fprintf(stderr, "[darwinn] tensor buffers allocated (in=%zu out=%zu bytes)\n",
                INPUT_BYTES, OUTPUT_BYTES);

        ready = true;
        return true;
    }

    bool register_model(const char* path) {
        std::vector<uint8_t> dgc0;
        if (!load_dgc0(path, dgc0)) return false;

        // Phase 18 Frida trace confirmed: GCamera uses RegisterGraphByBuffer6 (not RegisterGraph6).
        // Layout: x0=vdev x1=ahb(DGC0) x2=null x3=1(required) x4=0 x5=0 x6=0 x7=&out_graph.
        // x3=1 is the required flag (same role as x5=1 in RegisterGraph6).
        AHardwareBuffer* dgc0_ahb = ahb_alloc(dgc0.size());
        if (!dgc0_ahb) return false;

        if (!ahb_write(dgc0_ahb, dgc0.data(), dgc0.size())) {
            AHardwareBuffer_release(dgc0_ahb);
            return false;
        }
        fprintf(stderr, "[darwinn] DGC0 AHardwareBuffer @ %p (%zu bytes)\n",
                (void*)dgc0_ahb, dgc0.size());

        sym.RegisterGraphByBuffer6(vdev, dgc0_ahb, nullptr, 1, 0, 0, 0, &graph,
                                   nullptr, nullptr, 0, 0, 0, nullptr);
        AHardwareBuffer_release(dgc0_ahb);   // driver has its own reference after registration

        if (!graph) {
            fprintf(stderr, "[darwinn] RegisterGraphByBuffer6 returned null graph\n");
            fprintf(stderr, "[darwinn] Check: DGC0 FlatBuffer valid? VirtualDevice vtable intact?\n");
            fprintf(stderr, "[darwinn] If SIGABRT here: likely vtable null (SELinux context wrong).\n");
            return false;
        }
        fprintf(stderr, "[darwinn] Graph registered @ %p\n", graph);

        // Tensor specs (CONFIRMED exports, signatures [INFERRED])
        in_tensor  = sym.GetInputTensor(graph, 0);
        out_tensor = sym.GetOutputTensor(graph, 0);
        fprintf(stderr, "[darwinn] input_tensor=%p  output_tensor=%p\n", in_tensor, out_tensor);

        if (!in_tensor || !out_tensor) {
            fprintf(stderr, "[darwinn] tensor spec null — graph may have different tensor count\n");
            return false;
        }
        return true;
    }

    // Run one inference frame. Returns latency in microseconds, -1 on error.
    int64_t infer(const uint8_t* input, size_t input_len,
                  uint8_t* output, size_t output_len) {
        if (!ready) return -1;
        if (input_len > INPUT_BYTES)  input_len  = INPUT_BYTES;
        if (output_len > OUTPUT_BYTES) output_len = OUTPUT_BYTES;

        // Fill input AHardwareBuffer (MTE-safe: lock/write/unlock)
        if (!ahb_write(in_buf, input, input_len)) return -1;

        // CreateInferenceRequest — CONFIRMED 3-arg form (Phase 15 disassembly)
        DarwinnInferenceRequest* req = nullptr;
        sym.CreateInferenceRequest(vdev, graph, &req);
        if (!req) {
            fprintf(stderr, "[darwinn] CreateInferenceRequest returned null\n");
            fprintf(stderr, "[darwinn] If vtable was null above, this is expected.\n");
            fprintf(stderr, "[darwinn] Requires correct SELinux context.\n");
            return -1;
        }

        // AddInput / AddOutput [INFERRED signatures]
        // If runner aborts here, the signature (req, tensor_spec, ahb, offset)
        // may need adjustment. Check: aarch64-linux-android-objdump -d libedgetpu_util.so
        // and look at DarwinnApi2_InferenceRequest_AddInput for argument count.
        int r;
        r = sym.AddInput(req, in_tensor, in_buf, 0);
        if (r != 0) {
            fprintf(stderr, "[darwinn] AddInput error: %d\n", r);
            fprintf(stderr, "[darwinn] [INFERRED SIGNATURE] — adjust AddInput typedef if wrong\n");
            return -1;
        }
        r = sym.AddOutput(req, out_tensor, out_buf, 0);
        if (r != 0) {
            fprintf(stderr, "[darwinn] AddOutput error: %d\n", r);
            return -1;
        }

        // Submit with completion callback
        CompCtx ctx;
        sem_init(&ctx.sem, 0, 0);
        ctx.status = 0;

        auto t0 = std::chrono::steady_clock::now();

        r = sym.Submit(vdev, req, submit_cb, &ctx);
        if (r != 0) {
            fprintf(stderr, "[darwinn] Submit error: %d\n", r);
            sem_destroy(&ctx.sem);
            return -1;
        }

        // Primary wait: Request_Wait (optional export)
        if (sym.RequestWait) {
            r = sym.RequestWait(req, SUBMIT_WAIT_MS);
            if (r != 0)
                fprintf(stderr, "[darwinn] Request_Wait returned %d (timeout or unsupported)\n", r);
        }

        // Fallback: semaphore (fires from submit_cb if Submit accepts a callback)
        wait_completion(ctx, SUBMIT_WAIT_MS);
        sem_destroy(&ctx.sem);

        auto t1 = std::chrono::steady_clock::now();

        // Read output AHardwareBuffer (MTE-safe: lock after NPU write)
        if (!ahb_read(out_buf, output, output_len)) return -1;

        return std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    }

    void cleanup() {
        if (in_buf)  { AHardwareBuffer_release(in_buf);  in_buf  = nullptr; }
        if (out_buf) { AHardwareBuffer_release(out_buf); out_buf = nullptr; }
        if (lib)     { dlclose(lib); lib = nullptr; }
        ready = false;
    }

private:
    static void print_selinux_hint(const char* at) {
        fprintf(stderr, "[darwinn] SELinux CAVEAT at %s:\n", at);
        fprintf(stderr, "[darwinn]   Current context is likely u:r:shell:s0 or u:r:su:s0.\n");
        fprintf(stderr, "[darwinn]   DarWINN requires Binder access to edgetpu vendor service.\n");
        fprintf(stderr, "[darwinn]   Retry with:\n");
        fprintf(stderr, "[darwinn]     su -c 'runcon u:r:hal_camera_default:s0 /data/local/tmp/darwinn_runner --model <path>'\n");
        fprintf(stderr, "[darwinn]   Or permissive mode (testing only):\n");
        fprintf(stderr, "[darwinn]     su -c 'setenforce 0 && /data/local/tmp/darwinn_runner --model <path>'\n");
    }
};

// ── Socket server ─────────────────────────────────────────────────────────────
//
// Protocol (one request per connection):
//   Client → Server:  [4B uint32_le input_size][input_size bytes uint8 tensor]
//   Server → Client:  [4B int32_le status][4B uint32_le output_size]
//                     [output_size bytes][8B int64_le latency_us]
//
//   status == 0: success. status < 0: error (output_size will be 0).
//   latency_us: wall time from Submit call to output buffer read.
//   For portrait model: expect ~62000 µs (62ms) on first call; faster when cached.

static void send_response(int fd, int32_t status, const uint8_t* out, uint32_t out_len, int64_t lat) {
    write(fd, &status,  sizeof(status));
    write(fd, &out_len, sizeof(out_len));
    if (out_len > 0 && out) write(fd, out, out_len);
    write(fd, &lat,     sizeof(lat));
}

static void handle_client(int fd, Runner& runner) {
    uint32_t in_size = 0;
    if (read(fd, &in_size, 4) != 4 || in_size == 0 || in_size > INPUT_BYTES) {
        fprintf(stderr, "[server] invalid input_size: %u\n", in_size);
        int32_t err = -1; uint32_t z = 0; int64_t l = 0;
        send_response(fd, err, nullptr, 0, 0);
        close(fd);
        return;
    }

    std::vector<uint8_t> input(in_size);
    size_t got = 0;
    while (got < in_size) {
        ssize_t n = read(fd, input.data() + got, in_size - got);
        if (n <= 0) { close(fd); return; }
        got += (size_t)n;
    }

    std::vector<uint8_t> output(OUTPUT_BYTES, 0);
    int64_t lat = runner.infer(input.data(), in_size, output.data(), OUTPUT_BYTES);

    if (lat < 0) {
        send_response(fd, -1, nullptr, 0, 0);
    } else {
        send_response(fd, 0, output.data(), (uint32_t)OUTPUT_BYTES, lat);
    }
    close(fd);
}

static void run_server(Runner& runner, const char* sock_path) {
    unlink(sock_path);

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return; }

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "[server] bind %s failed: %s\n", sock_path, strerror(errno));
        close(srv);
        return;
    }
    chmod(sock_path, 0777);  // allow non-root callers (Termux, adb shell)
    listen(srv, 4);
    fprintf(stderr, "[server] listening on %s\n", sock_path);
    fprintf(stderr, "[server] protocol: [4B input_size][input] → [4B status][4B out_size][output][8B latency_us]\n");

    while (true) {
        int fd = accept(srv, nullptr, nullptr);
        if (fd < 0) continue;
        handle_client(fd, runner);
    }
    close(srv);
}

// ── main ──────────────────────────────────────────────────────────────────────

static void usage(const char* argv0) {
    fprintf(stderr,
        "DarWINN Standalone NPU Runner — Pixel 7 / Tensor G2 / Android 14\n"
        "\n"
        "Usage: %s --model <path.dgc0> [--socket <path>] [--help]\n"
        "\n"
        "  --model   DGC0 FlatBuffer file (raw or from edgetpu cache).\n"
        "            Cache: /data/vendor/edgetpu/cache/<hash>  (59-byte header stripped automatically)\n"
        "            Raw:   extract from TFLite custom_options via extract_edgetpu_graph.py\n"
        "  --socket  Unix socket path (default: %s)\n"
        "\n"
        "Root + correct SELinux context required. CreateVirtualDevice from plain shell\n"
        "returns an uninitialized VirtualDevice (null vtable — Phase 15 confirmed).\n"
        "\n"
        "Recommended launch:\n"
        "  su -c 'runcon u:r:hal_camera_default:s0 /data/local/tmp/darwinn_runner --model /path/to/model.dgc0'\n"
        "\n"
        "Test model: pull after any portrait camera session:\n"
        "  adb shell \"su -c 'ls /data/vendor/edgetpu/cache/'\"\n"
        "  adb pull /data/vendor/edgetpu/cache/<hash>  model.bin\n",
        argv0, DEFAULT_SOCK);
}

int main(int argc, char* argv[]) {
#ifndef PR_SET_TAGGED_ADDR_CTRL
#define PR_SET_TAGGED_ADDR_CTRL 55
#endif
    // Try to disable tagged address checking so Bionic's free() does not validate tags.
    if (prctl(PR_SET_TAGGED_ADDR_CTRL, 0, 0, 0, 0) == 0) {
        fprintf(stderr, "[darwinn] Successfully disabled tagged address control\n");
    } else {
        fprintf(stderr, "[darwinn] Failed to disable tagged address control: %s\n", strerror(errno));
    }

    const char* model_path = nullptr;
    const char* sock_path  = DEFAULT_SOCK;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--model")  && i + 1 < argc) { model_path = argv[++i]; continue; }
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) { sock_path  = argv[++i]; continue; }
        if (!strcmp(argv[i], "--help"))  { usage(argv[0]); return 0; }
    }

    if (!model_path) {
        fprintf(stderr, "error: --model is required\n\n");
        usage(argv[0]);
        return 1;
    }

    fprintf(stderr, "[darwinn] DarWINN Standalone Runner — Pixel 7 / Tensor G2\n");
    fprintf(stderr, "[darwinn] model:  %s\n", model_path);
    fprintf(stderr, "[darwinn] socket: %s\n", sock_path);
    fprintf(stderr, "[darwinn] lib:    %s\n", LIB_PATH);

    Runner runner;
    if (!runner.init(model_path)) {
        fprintf(stderr, "[darwinn] init failed — see errors above\n");
        runner.cleanup();
        return 1;
    }

    fprintf(stderr, "[darwinn] ready — accepting inference requests\n");
    run_server(runner, sock_path);

    runner.cleanup();
    return 0;
}
