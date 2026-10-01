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
#include <signal.h>
#include <ucontext.h>
#include <pthread.h>

// SPIN_CFG: continuously re-assert the config native-int4 bit at base+off so every copy/read
// downstream of the config-load inherits native (keeps the whole pipeline consistently native).
static volatile int g_spin_run = 0;
static volatile unsigned char* g_spin_addr = 0;
static void* spinner_fn(void* a) { (void)a; while (g_spin_run) { *g_spin_addr |= 1u; } return 0; }

// FORCE_2EC: every +0x2ec write site {file-offset, base-reg}. Forcing each to store 1 makes the
// whole config/pass-config family native throughout (config-load root + all copies), regardless of
// the per-run heap address of the config object.
static const struct { unsigned long off; int reg; } g_2ec[] = {
    {0xa4d5bc,19},{0xf94bb8,19},{0x1799d44,22},{0x1bd061c,19},{0x277cd20,19},
    {0x27b3478,19},{0x27b3b6c,19},{0x35c74e8,20},{0x35d459c,20},{0x37bb014,21},{0x3867f98,20}
};
#define N_2EC (sizeof(g_2ec)/sizeof(g_2ec[0]))

// --- dynamic caller trace (TRACE_OFF): patch a func entry to brk, catch LR = caller ---
static void* g_trace_base;
static int g_ptm = 0;
static int g_tmforce = -1;
static unsigned int g_tm_log[8000];
static int g_tm_n = 0;
static volatile unsigned int g_maplog[16386];  // [0]=count, then (E,result) pairs
static volatile unsigned int g_hookflag = 0;    // set by HOOKREACH trampoline when the target is entered
static int g_costlog = 0;
static void bt_handler(int sig, siginfo_t* si, void* uc_) {
    ucontext_t* uc=(ucontext_t*)uc_;
    unsigned long* regs=(unsigned long*)uc->uc_mcontext.regs;
    uintptr_t b=(uintptr_t)g_trace_base;
    unsigned long pc=uc->uc_mcontext.pc, sp=uc->uc_mcontext.sp;
    unsigned long lo=b+0x1000, hi=b+0x3d00000;   // .so executable range
    dprintf(2,"\n=== BTRACE sig=%d sp=%lx (scan stack for .so return addrs) ===\n", sig, sp);
    int shown=0;
    for(unsigned long p=sp; p<sp+0x20000 && shown<40; p+=8){
        unsigned long v=*(unsigned long*)p;
        if(v>=lo && v<hi){
            unsigned int prev=*(unsigned int*)(v-4);      // instr before the return addr
            if((prev&0xfc000000)==0x94000000 || (prev&0xfffffc1f)==0xd63f0000) // BL or BLR
                dprintf(2,"  +0x%lx\n", v-b);
            shown++;
        }
    }
    _exit(42);
}
static void trap_handler(int sig, siginfo_t* si, void* uc_) {
    ucontext_t* uc = (ucontext_t*)uc_;
    unsigned long* regs = uc->uc_mcontext.regs;
    uintptr_t b = (uintptr_t)g_trace_base;
    unsigned long pc = uc->uc_mcontext.pc, lr = regs[30], sp = uc->uc_mcontext.sp;
    // COSTLOG: at cost fn 0x2600038, log (w0,w1)=cell pair, emulate 'sub sp,sp,0x90', continue at pc+4.
    if (g_costlog && pc == b + 0x2600038) {
        unsigned int cnt = g_maplog[0];
        if (cnt < 4000) { g_maplog[1+cnt*2]=(unsigned)(regs[0]&0xff); g_maplog[2+cnt*2]=(unsigned)(regs[1]&0xff); g_maplog[0]=cnt+1; }
        uc->uc_mcontext.sp -= 0x90;        // emulate sub sp,sp,#0x90
        uc->uc_mcontext.pc = pc + 4;
        return;
    }
    // PROBE_TM: log the element-code->cell-code mapper 0x384210c input/output, then emulate+return.
    if (g_ptm && pc == b + 0x384210c) {
        unsigned long E = regs[0] & 0xffffffffUL;
        unsigned char* tbl = (unsigned char*)(b + 0x2a7218);
        unsigned int res = (E < 0x10000) ? tbl[E] : 0xAA;
        if (g_tmforce >= 0 && (res==0 || res==0x13 || res==0x14 || res==0xAA)) res = (unsigned)g_tmforce;
        if (g_ptm==2 && g_tm_n < 4000) {   // verbose: log to a ring in a preallocated buffer (async-safe-ish)
            g_tm_log[g_tm_n*2]=(unsigned)E; g_tm_log[g_tm_n*2+1]=res; g_tm_n++;
        }
        regs[0] = res;
        uc->uc_mcontext.pc = lr;   // emulate: return looked-up (or forced) code to caller
        return;
    }
    // FORCE_MODE: at the gate 0xf2d858, write detected native mode into the pass mode field + resume.
    // gate = `ldr w8,[x19,#0x2a8]`; we set [x19+0x2a8]=detected, w8=detected, skip the brk (pc+4).
    // FULL_NATIVE: force config-cap native (0x18669a4) AND the embedding-lookup-rescale pass
    // option init true (0x17a7cf4 = strb wzr,[sp,#0xc] -> write 1 instead). Both dispatched by PC.
    if (getenv("FULL_NATIVE")) {
        if (pc == b + 0x18669a4) {                 // config native-cap read
            unsigned long cfg = regs[8];
            if (cfg > 0x1000) *(volatile unsigned char*)(cfg + 0x2ec) |= 1u;
            regs[8] = 1; uc->uc_mcontext.pc = pc + 4; return;
        }
        // after embedding-option ctor returns (x20=pass): force Option value (pass+0x308 = Option+0xd8) = true.
        // we replaced `adrp x8, 0x107000` (site1) / `adrp x8, 0x11f000` (site2) -> emulate the adrp.
        if (pc == b + 0x17a7d00) {
            unsigned long p = regs[20];
            if (getenv("OPT_DUMP") && p > 0x1000) {
                unsigned long ptr300 = *(unsigned long*)(p + 0x300);   // [Option+0xd0] pointer
                dprintf(2, "  OPT x20=%p  [+0x300]=%p  byte[+0x308]=%02x\n", (void*)p, (void*)ptr300, *(unsigned char*)(p+0x308));
                if (ptr300 > 0x1000 && ptr300 < 0x8000000000UL)
                    dprintf(2, "  *[+0x300] bytes: %02x %02x %02x %02x  (value may live here)\n",
                            *(unsigned char*)ptr300, *(unsigned char*)(ptr300+1), *(unsigned char*)(ptr300+2), *(unsigned char*)(ptr300+3));
            }
            if (p > 0x1000) *(volatile unsigned char*)(p + 0x308) = 1u;
            regs[8] = b + 0x107000; uc->uc_mcontext.pc = pc + 4; return;
        }
        if (pc == b + 0x181749c) {                 // 2nd pass instance, post-ctor (same adrp)
            if (regs[20] > 0x1000) *(volatile unsigned char*)(regs[20] + 0x308) = 1u;
            regs[8] = b + 0x107000; uc->uc_mcontext.pc = pc + 4; return;
        }
        uc->uc_mcontext.pc = pc + 4; return;
    }
    // FORCE_2EC: at any +0x2ec write site, store native (1) instead of 0/copy, then step over.
    if (getenv("FORCE_2EC")) {
        for (unsigned i=0;i<N_2EC;i++) if (pc == b + g_2ec[i].off) {
            unsigned long obj = regs[g_2ec[i].reg];
            if (obj > 0x1000) *(volatile unsigned char*)(obj + 0x2ec) = 1u;
            break;
        }
        uc->uc_mcontext.pc = pc + 4; return;
    }
    // NATIVE_ALL: force BOTH the config-capability (0x18669a4) AND the mode gate (0xf2d858) native,
    // dispatched by PC, so the whole pipeline is consistently native from setup through the pass.
    if (getenv("NATIVE_ALL")) {
        if (pc == b + 0x18669a4) {               // ldrb w8,[x8,#0x2ec] : config native cap
            unsigned long cfg = regs[8];
            if (cfg > 0x1000) *(volatile unsigned char*)(cfg + 0x2ec) |= 1u;
            regs[8] = 1; uc->uc_mcontext.pc = pc + 4; return;
        }
        if (pc == b + 0xf2d858) {                // ldr w8,[x19,#0x2a8] : mode field
            unsigned long obj = regs[19];
            { const char* rm = getenv("RHS_MODE"); unsigned int mode = rm ? (unsigned int)strtoul(rm,0,0) : 7u;
              unsigned int prev = (obj>0x1000)? *(volatile unsigned int*)(obj+0x2a8):0xffffffffu;
              if (obj > 0x1000) *(volatile unsigned int*)(obj + 0x2a8) = mode;
              regs[8] = mode; dprintf(2,"MODEGATE_FIRE detected_prev=%u forced_to=%u\n",prev,mode); } uc->uc_mcontext.pc = pc + 4; return;
        }
        uc->uc_mcontext.pc = pc + 4; return;     // any other armed brk: just step over
    }
    // FORCE_CFG: at a config-capability read `ldrb w8,[x8,#0x2ec]`, set the native bit in the
    // config data AND the loaded reg, then resume. "force at source" = fixes all downstream reads.
    const char* fc = getenv("FORCE_CFG");
    if (fc) {
        unsigned long cfg = regs[8];
        static int cfg_dumped = 0;
        if (getenv("CFG_DUMP") && !cfg_dumped && cfg > 0x1000) {
            cfg_dumped = 1;
            dprintf(2, "  CFG_DUMP @%p (native bit at +0x2ec):\n", (void*)cfg);
            for (int q = 0; q < 0x400; q += 32) {
                dprintf(2, "   +%03x:", q);
                for (int r = 0; r < 32; r += 4) dprintf(2, " %08x", *(volatile unsigned int*)(cfg + q + r));
                dprintf(2, "\n");
            }
        }
        // CFG_FORCE_ALL: broadly set config capability bytes to test config-gating of the 3 passes.
        if (getenv("CFG_FORCE_ALL") && cfg > 0x1000) {
            for (int q = 0x2c0; q < 0x340; q++) *(volatile unsigned char*)(cfg + q) |= 1u;
        }
        if (cfg > 0x1000) *(volatile unsigned char*)(cfg + 0x2ec) |= 1u; // set native bit in config
        regs[8] = 1;
        uc->uc_mcontext.pc = pc + 4;
        return;
    }
    const char* fm = getenv("FORCE_MODE");
    if (fm) {
        unsigned long obj = regs[19];
        unsigned int det = (unsigned int)atoi(fm); // e.g. 7
        if (obj > 0x1000) {
            *(volatile unsigned int*)(obj + 0x2a8) = det;   // fix the mode field (all reads)
            regs[8] = det;                                   // the ldr's result register
        }
        uc->uc_mcontext.pc = pc + 4;                         // skip past the brk, continue
        return;
    }
    dprintf(2, "\n=== TRAP pc=+0x%lx  x30/LR=+0x%lx ===\n", pc - b, lr - b);
    const char* fenv = getenv("DUMP_FIELD"); // "REG" e.g. "19" -> dump +0x1c8,+0x2a8 and scan for 5/7
    if (fenv) {
        int reg=atoi(fenv); unsigned long obj=regs[reg];
        if (obj>0x1000) {
            dprintf(2,"  x%d=%p  [+0x1c8]=0x%x (cap)  [+0x2a8]=0x%x (mode)\n",
                    reg,(void*)obj,*(unsigned char*)(obj+0x1c8),*(unsigned int*)(obj+0x2a8));
            dprintf(2,"  scan obj for word==5 or 7 in [0,0x500):");
            for(unsigned long o=0;o<0x500;o+=4){ unsigned int v=*(unsigned int*)(obj+o); if(v==5||v==7) dprintf(2," +0x%lx=%u",o,v); }
            dprintf(2,"\n");
        }
    }
    dprintf(2, "  stack-scan return addresses into lib (call chain, innermost first):\n");
    unsigned long* s = (unsigned long*)sp;
    for (int i = 0; i < 0x1000; i++) {
        unsigned long v = s[i];
        if (v > b && v < b + 0x4000000) {
            unsigned int op = *(unsigned int*)(v - 4);
            if ((op & 0xfc000000u) == 0x94000000u || (op & 0xfffffc1fu) == 0xd63f0000u)
                dprintf(2, "    +0x%lx\n", v - b);
        }
    }
    _exit(42);
}

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
    int hw_soc_rev = getenv("UID") ? atoi(getenv("UID")) : get_int_property("ro.boot.hw.soc.rev", 1);

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

    int hwver = getenv("HWVER") ? atoi(getenv("HWVER")) : 16;
    // MOVZ Wd,#imm = 0x52800000 | (imm<<5) | Rd ;  patch0 uses w20, patch1 uses w0
    uint32_t enc20 = 0x52800000u | ((uint32_t)hwver << 5) | 20u;
    uint32_t enc0  = 0x52800000u | ((uint32_t)hwver << 5) | 0u;
    g_patches[0].newb[0]=enc20&0xff; g_patches[0].newb[1]=(enc20>>8)&0xff; g_patches[0].newb[2]=(enc20>>16)&0xff; g_patches[0].newb[3]=(enc20>>24)&0xff;
    g_patches[1].newb[0]=enc0&0xff;  g_patches[1].newb[1]=(enc0>>8)&0xff;  g_patches[1].newb[2]=(enc0>>16)&0xff;  g_patches[1].newb[3]=(enc0>>24)&0xff;
    printf("  HWVER=%d (enc20=%08x enc0=%08x)\n", hwver, enc20, enc0);
    // optional extra ELF patch(es) from env for the int4 gate:
    //   EXTRA_OFF="0xNNNN" EXTRA_NEW="aabbccdd" (hex bytes, any length)
    const char* eoff=getenv("EXTRA_OFF"); const char* enew=getenv("EXTRA_NEW");
    if(eoff&&enew){
        size_t xo=strtoull(eoff,0,16); size_t n=strlen(enew)/2;
        uint8_t nb[64]; for(size_t k=0;k<n&&k<64;k++){ unsigned v; sscanf(enew+2*k,"%2x",&v); nb[k]=(uint8_t)v; }
        uintptr_t addr=(uintptr_t)g_lookup.base+xo, page=addr&~0xfffULL;
        size_t plen=((addr+n+0xfff)&~0xfffULL)-page;
        printf("  EXTRA patch @0x%zx: before=", xo);
        for(size_t k=0;k<n;k++) printf("%02x ",((uint8_t*)addr)[k]);
        mprotect((void*)page,plen,PROT_READ|PROT_WRITE|PROT_EXEC);
        memcpy((void*)addr,nb,n); __builtin___clear_cache((char*)addr,(char*)addr+n);
        mprotect((void*)page,plen,PROT_READ|PROT_EXEC);
        printf(" after="); for(size_t k=0;k<n;k++) printf("%02x ",((uint8_t*)addr)[k]); printf("\n");
    }
    // REMAP: redirect the cost-type mapper 0x384210c to a cave thunk that does the real table lookup
    // then remaps INVALID cell-codes (0,0x13,0x14) -> REMAP (default 5=UINT1). REMAP_FN default 0x384210c.
    if(getenv("REMAP")){
        unsigned newcode = (unsigned)strtoul(getenv("REMAP"),0,0);
        uintptr_t fn = (uintptr_t)g_lookup.base + (getenv("REMAP_FN")?strtoull(getenv("REMAP_FN"),0,16):0x384210c);
        uintptr_t tbl = (uintptr_t)g_lookup.base + 0x2a7218;   // internal-elemtype -> cell-code table
        // mmap a cave within +-120MB of fn (B has +-128MB reach). Hints are advisory, so scan
        // candidate slots with MAP_FIXED_NOREPLACE until one lands (and stays) in range.
        void* cave = MAP_FAILED;
        for(long delta=0x100000; delta<=0x7000000; delta+=0x100000){
            uintptr_t cand = (fn & ~0xfffULL) - (uintptr_t)delta;
            void* p = mmap((void*)cand, 0x1000, PROT_READ|PROT_WRITE|PROT_EXEC,
                           MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
            if(p!=MAP_FAILED && p==(void*)cand){ cave=p; break; }
            if(p!=MAP_FAILED) munmap(p,0x1000);
            cand = (fn & ~0xfffULL) + (uintptr_t)delta;
            p = mmap((void*)cand, 0x1000, PROT_READ|PROT_WRITE|PROT_EXEC,
                     MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
            if(p!=MAP_FAILED && p==(void*)cand){ cave=p; break; }
            if(p!=MAP_FAILED) munmap(p,0x1000);
        }
        if(cave==MAP_FAILED){ printf("  REMAP: no cave slot in range\n"); }
        else {
            long d = (long)((uintptr_t)cave) - (long)fn;
            if(d> (120L<<20) || d< -(120L<<20)){ printf("  REMAP: cave too far (%ld MB)\n", d>>20); }
            else {
                uint32_t* t=(uint32_t*)cave; int i=0;
                if(getenv("MAPLOG")){   // thunk: log (E,result) to g_maplog, pass through unchanged
                    uintptr_t lb=(uintptr_t)&g_maplog[0];
                    t[i++]=0xD2800008u | (((uint32_t)(tbl>>0)&0xffff)<<5);
                    t[i++]=0xF2A00008u | (((uint32_t)(tbl>>16)&0xffff)<<5);
                    t[i++]=0xF2C00008u | (((uint32_t)(tbl>>32)&0xffff)<<5);
                    t[i++]=0xF2E00008u | (((uint32_t)(tbl>>48)&0xffff)<<5);
                    t[i++]=0x38604909u;                                        // ldrb w9,[x8,w0,uxtw]
                    t[i++]=0xD280000Au | (((uint32_t)(lb>>0)&0xffff)<<5);      // movz x10,#lo
                    t[i++]=0xF2A0000Au | (((uint32_t)(lb>>16)&0xffff)<<5);
                    t[i++]=0xF2C0000Au | (((uint32_t)(lb>>32)&0xffff)<<5);
                    t[i++]=0xF2E0000Au | (((uint32_t)(lb>>48)&0xffff)<<5);
                    t[i++]=0xB940014Bu;                                        // ldr w11,[x10]
                    t[i++]=0x713E801Fu;                                        // cmp w11,#4000
                    t[i++]=0x540000E2u;                                        // b.hs +7 (skip store)
                    t[i++]=0x9100114Cu;                                        // add x12,x10,#4
                    t[i++]=0x8B0B0D8Cu;                                        // add x12,x12,x11,lsl#3
                    t[i++]=0xB9000180u;                                        // str w0,[x12]
                    t[i++]=0xB9000589u;                                        // str w9,[x12,#4]
                    t[i++]=0x1100056Bu;                                        // add w11,w11,#1
                    t[i++]=0xB900014Bu;                                        // str w11,[x10]
                    t[i++]=0x2A0903E0u;                                        // mov w0,w9
                    t[i++]=0xD65F03C0u;                                        // ret
                    __builtin___clear_cache((char*)cave,(char*)cave+i*4);
                    uintptr_t pg=fn&~0xfffULL; mprotect((void*)pg,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC);
                    long bo=((long)((uintptr_t)cave)-(long)fn)>>2; *(uint32_t*)fn=0x14000000u|((uint32_t)bo&0x03ffffff);
                    __builtin___clear_cache((char*)fn,(char*)fn+4); mprotect((void*)pg,0x2000,PROT_READ|PROT_EXEC);
                    printf("  MAPLOG: fn@0x%zx -> cave %p\n",(size_t)(fn-(uintptr_t)g_lookup.base),cave);
                    goto remap_done;
                }
                if(getenv("REMAP_ALL")){   // thunk = movz w0,#newcode ; ret  (unconditional, = inline global force)
                    t[0]=0x52800000u | ((newcode&0xffff)<<5) | 0u;
                    t[1]=0xD65F03C0u;
                    __builtin___clear_cache((char*)cave,(char*)cave+8);
                    uintptr_t pg=fn&~0xfffULL; mprotect((void*)pg,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC);
                    long bo=((long)((uintptr_t)cave)-(long)fn)>>2; *(uint32_t*)fn=0x14000000u|((uint32_t)bo&0x03ffffff);
                    __builtin___clear_cache((char*)fn,(char*)fn+4); mprotect((void*)pg,0x2000,PROT_READ|PROT_EXEC);
                    printf("  REMAP_ALL: fn@0x%zx -> cave %p (ALL->%u)\n",(size_t)(fn-(uintptr_t)g_lookup.base),cave,newcode);
                    goto remap_done;
                }
                // movz/movk x8 = tbl (absolute 64-bit)
                t[i++]=0xD2800008u | (((uint32_t)(tbl>>0)&0xffff)<<5);          // movz x8,#lo
                t[i++]=0xF2A00008u | (((uint32_t)(tbl>>16)&0xffff)<<5);         // movk x8,#b1,lsl16
                t[i++]=0xF2C00008u | (((uint32_t)(tbl>>32)&0xffff)<<5);         // movk x8,#b2,lsl32
                t[i++]=0xF2E00008u | (((uint32_t)(tbl>>48)&0xffff)<<5);         // movk x8,#b3,lsl48
                t[i++]=0x38604900u;                                            // ldrb w0,[x8,w0,uxtw]
                // remap any code NOT in valid cost-set {1,2,3,4,5,7,8,9,10,11,12} -> newcode.
                // triggers: code==0 || code==6 || code>12. set5 at index 12.
                t[i++]=0x7100001Fu;                                            // cmp w0,#0
                t[i++]=0x54000000u | ((uint32_t)((12-6)&0x7ffff)<<5) | 0u;     // b.eq set5
                t[i++]=0x7100001Fu | (6u<<10);                                // cmp w0,#6
                t[i++]=0x54000000u | ((uint32_t)((12-8)&0x7ffff)<<5) | 0u;     // b.eq set5
                t[i++]=0x7100001Fu | (12u<<10);                               // cmp w0,#12
                t[i++]=0x54000000u | ((uint32_t)((12-10)&0x7ffff)<<5) | 8u;    // b.hi set5 (unsigned >)
                t[i++]=0xD65F03C0u;                                            // ret (valid: keep w0)
                t[i++]=0x52800000u | ((newcode&0xffff)<<5) | 0u;              // movz w0,#newcode
                t[i++]=0xD65F03C0u;                                            // ret
                __builtin___clear_cache((char*)cave,(char*)cave+i*4);
                // patch fn -> B cave
                uintptr_t page=fn&~0xfffULL;
                mprotect((void*)page,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC);
                long boff=((long)((uintptr_t)cave)-(long)fn)>>2;
                *(uint32_t*)fn = 0x14000000u | ((uint32_t)boff & 0x03ffffff);
                __builtin___clear_cache((char*)fn,(char*)fn+4);
                mprotect((void*)page,0x2000,PROT_READ|PROT_EXEC);
                printf("  REMAP: fn@0x%zx -> cave %p (invalid{0,6,>12}->%u), B off=%ld\n",
                       (size_t)(fn-(uintptr_t)g_lookup.base), cave, newcode, boff);
            }
            remap_done: ;
        }
    }
    // BTRACE: install SIGABRT/SIGSEGV handler that walks the FP chain and prints base-relative offsets.
    if(getenv("BTRACE")){
        g_trace_base=g_lookup.base;
        struct sigaction sb; memset(&sb,0,sizeof(sb)); sb.sa_sigaction=bt_handler; sb.sa_flags=SA_SIGINFO;
        sigaction(SIGABRT,&sb,0); sigaction(SIGSEGV,&sb,0); sigaction(SIGBUS,&sb,0);
        printf("  BTRACE: armed abort/segv backtrace\n");
    }
    // COSTLOG: brk at cost fn 0x2600038; handler logs (w0,w1)=cell pair, emulates 'sub sp,sp,0x90', pc+=4.
    if(getenv("COSTLOG")){
        g_ptm=0; g_trace_base=g_lookup.base;
        struct sigaction sa; memset(&sa,0,sizeof(sa)); sa.sa_sigaction=trap_handler; sa.sa_flags=SA_SIGINFO;
        sigaction(SIGTRAP,&sa,0); sigaction(SIGILL,&sa,0);
        uint32_t brk=0xd4200000u;
        uintptr_t addr=(uintptr_t)g_lookup.base+0x2600038, page=addr&~0xfffULL;
        mprotect((void*)page,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC);
        memcpy((void*)addr,&brk,4); __builtin___clear_cache((char*)addr,(char*)addr+4);
        mprotect((void*)page,0x2000,PROT_READ|PROT_EXEC);
        g_costlog=1; printf("  COSTLOG: armed brk @0x2600038\n");
    }
    // HOOKREACH="0xNNNN,0xORIG" : trampoline that sets g_hookflag when the target fn is entered, then
    // runs the (PC-independent) original first instruction ORIG and jumps back to target+4. No brk.
    if(getenv("HOOKREACH")){
        char hb[64]; strncpy(hb,getenv("HOOKREACH"),63); hb[63]=0;
        char* c=strchr(hb,','); unsigned long orig=0;
        if(c){*c=0; orig=strtoul(c+1,0,16);}
        uintptr_t fn=(uintptr_t)g_lookup.base+strtoull(hb,0,16);
        void* cave=MAP_FAILED;
        for(long dl=0x100000; dl<=0x7000000; dl+=0x100000){
            uintptr_t cd=(fn&~0xfffULL)-(uintptr_t)dl;
            void* p=mmap((void*)cd,0x1000,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
            if(p!=MAP_FAILED&&p==(void*)cd){cave=p;break;} if(p!=MAP_FAILED)munmap(p,0x1000);
            cd=(fn&~0xfffULL)+(uintptr_t)dl;
            p=mmap((void*)cd,0x1000,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
            if(p!=MAP_FAILED&&p==(void*)cd){cave=p;break;} if(p!=MAP_FAILED)munmap(p,0x1000);
        }
        if(cave!=MAP_FAILED){
            uintptr_t fl=(uintptr_t)&g_hookflag; uint32_t* t=(uint32_t*)cave; int i=0;
            t[i++]=0xD2800010u|(((uint32_t)(fl>>0)&0xffff)<<5);   // movz x16,#lo
            t[i++]=0xF2A00010u|(((uint32_t)(fl>>16)&0xffff)<<5);
            t[i++]=0xF2C00010u|(((uint32_t)(fl>>32)&0xffff)<<5);
            t[i++]=0xF2E00010u|(((uint32_t)(fl>>48)&0xffff)<<5);
            t[i++]=0x52800031u;                                  // movz w17,#1
            t[i++]=0xB9000211u;                                  // str w17,[x16]
            t[i++]=(uint32_t)orig;                               // relocated original first instr
            long bo=((long)((uintptr_t)&t[i])-(long)(fn+4))>>2;  // B back to fn+4
            t[i++]=0x14000000u|((uint32_t)bo&0x03ffffff);
            __builtin___clear_cache((char*)cave,(char*)cave+i*4);
            uintptr_t pg=fn&~0xfffULL; mprotect((void*)pg,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC);
            long b2=((long)((uintptr_t)cave)-(long)fn)>>2; *(uint32_t*)fn=0x14000000u|((uint32_t)b2&0x03ffffff);
            __builtin___clear_cache((char*)fn,(char*)fn+4); mprotect((void*)pg,0x2000,PROT_READ|PROT_EXEC);
            printf("  HOOKREACH: fn@0x%zx orig=%08lx cave=%p\n",(size_t)(fn-(uintptr_t)g_lookup.base),orig,cave);
        } else printf("  HOOKREACH: no cave\n");
    }
    // dynamic trace: TRACE_OFF="0xNNNN" -> patch entry to brk #0, handler prints caller LR
    if(getenv("EMB_ON")){
        // Force one or more .bss gate globals to 1. EMB_GOFF = comma-separated hex offsets.
        char buf[256]; const char* e=getenv("EMB_GOFF");
        strncpy(buf, e?e:"0x3d850a8", sizeof(buf)-1); buf[sizeof(buf)-1]=0;
        for(char* tok=strtok(buf,","); tok; tok=strtok(0,",")){
            unsigned long goff = strtoul(tok,0,16);
            unsigned char* g=(unsigned char*)g_lookup.base + goff;
            uintptr_t page=((uintptr_t)g)&~0xfffULL;
            mprotect((void*)page,0x2000,PROT_READ|PROT_WRITE);
            *g=1;
            printf("  EMB_ON: global @+%#lx = %d\n", goff, *g);
        }
    }
    const char* toff=getenv("TRACE_OFF");
    const char* nall=getenv("NATIVE_ALL");
    const char* f2ec=getenv("FORCE_2EC");
    const char* fnat=getenv("FULL_NATIVE");
    const char* ptm=getenv("PROBE_TM");
    if(toff || nall || f2ec || fnat || ptm){
        g_trace_base=g_lookup.base;
        struct sigaction sa; memset(&sa,0,sizeof(sa));
        sa.sa_sigaction=trap_handler; sa.sa_flags=SA_SIGINFO;
        sigaction(SIGTRAP,&sa,0); sigaction(SIGILL,&sa,0);
        uint32_t brk=0xd4200000u; // brk #0
        if(ptm){
            g_ptm=1; { const char* tf=getenv("TM_FORCE"); if(tf) g_tmforce=(int)strtoul(tf,0,0); }
            uintptr_t addr=(uintptr_t)g_lookup.base+0x384210c, page=addr&~0xfffULL;
            mprotect((void*)page,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC);
            memcpy((void*)addr,&brk,4); __builtin___clear_cache((char*)addr,(char*)addr+4);
            mprotect((void*)page,0x2000,PROT_READ|PROT_EXEC);
            printf("  PROBE_TM: armed mapper brk @0x384210c\n");
        }
        if(fnat){
            size_t fo[3]={0x18669a4,0x17a7d00,0x181749c};
            for(int k=0;k<3;k++){
                uintptr_t addr=(uintptr_t)g_lookup.base+fo[k], page=addr&~0xfffULL;
                mprotect((void*)page,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC);
                memcpy((void*)addr,&brk,4); __builtin___clear_cache((char*)addr,(char*)addr+4);
                mprotect((void*)page,0x2000,PROT_READ|PROT_EXEC);
            }
            printf("  FULL_NATIVE: armed config-cap + embedding-option init\n");
        } else if(f2ec){
            for(unsigned i=0;i<N_2EC;i++){
                uintptr_t addr=(uintptr_t)g_lookup.base+g_2ec[i].off, page=addr&~0xfffULL;
                mprotect((void*)page,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC);
                memcpy((void*)addr,&brk,4); __builtin___clear_cache((char*)addr,(char*)addr+4);
                mprotect((void*)page,0x2000,PROT_READ|PROT_EXEC);
            }
            printf("  FORCE_2EC: armed %zu +0x2ec write sites\n",(size_t)N_2EC);
        } else {
            size_t offs[3]; int no=0;
            if(nall){ offs[no++]=0x18669a4; offs[no++]=0xf2d858; }
            else offs[no++]=strtoull(toff,0,16);
            for(int k=0;k<no;k++){
                uintptr_t addr=(uintptr_t)g_lookup.base+offs[k], page=addr&~0xfffULL;
                mprotect((void*)page,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC);
                memcpy((void*)addr,&brk,4); __builtin___clear_cache((char*)addr,(char*)addr+4);
                mprotect((void*)page,0x2000,PROT_READ|PROT_EXEC);
                printf("  ARMED brk @0x%zx\n", offs[k]);
            }
        }
    }
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

    const char* spin=getenv("SPIN_CFG");
    if(spin){
        g_spin_addr=(volatile unsigned char*)((uintptr_t)g_lookup.base+strtoull(spin,0,16));
        g_spin_run=1;
        pthread_t th; pthread_create(&th,0,spinner_fn,0);
        printf("  SPIN_CFG: re-asserting native bit @base+0x%zx\n", (size_t)strtoull(spin,0,16));
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
    if (g_ptm==2) {
        printf("  [TM] %d mapper queries. distinct (E->res):\n", g_tm_n);
        for (int i=0;i<g_tm_n;i++){
            int dup=0; for(int j=0;j<i;j++) if(g_tm_log[j*2]==g_tm_log[i*2]&&g_tm_log[j*2+1]==g_tm_log[i*2+1]){dup=1;break;}
            if(!dup) printf("     E=%u(0x%x) -> 0x%02x\n", g_tm_log[i*2], g_tm_log[i*2], g_tm_log[i*2+1]);
        }
    }
    if(getenv("MAPLOG")){
        unsigned cnt=g_maplog[0]; if(cnt>4000)cnt=4000;
        printf("  [MAPLOG] %u mapper calls. distinct (E->cell):\n", cnt);
        for(unsigned a=0;a<cnt;a++){
            unsigned E=g_maplog[1+a*2], R=g_maplog[2+a*2]; int dup=0;
            for(unsigned bb=0;bb<a;bb++) if(g_maplog[1+bb*2]==E && g_maplog[2+bb*2]==R){dup=1;break;}
            if(!dup) printf("     E=%u(0x%x) -> cell 0x%02x\n", E, E, R);
        }
    }
    if(getenv("HOOKREACH")) printf("  [HOOKREACH] target entered = %s (flag=%u)\n", g_hookflag?"YES":"NO", g_hookflag);
    if(getenv("COSTLOG")){
        unsigned cnt=g_maplog[0]; if(cnt>4000)cnt=4000;
        printf("  [COSTLOG] %u cost-fn calls. distinct (cellA,cellB) pairs:\n", cnt);
        for(unsigned a=0;a<cnt;a++){
            unsigned A=g_maplog[1+a*2]&0xff, B=g_maplog[2+a*2]&0xff; int dup=0;
            for(unsigned bb=0;bb<a;bb++) if((g_maplog[1+bb*2]&0xff)==A&&(g_maplog[2+bb*2]&0xff)==B){dup=1;break;}
            if(!dup) printf("     (%u, %u)\n", A, B);
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
