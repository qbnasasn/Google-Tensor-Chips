/*
 * Tensor G3 (Rio) DarWINN bidirectional injection proof v3 — via Buffer API.
 *
 * Supersedes inject_v2_g3.js (which wrote raw bytes onto the opaque
 * DarwinnApi2_Buffer* handle itself and crashed GoogleCamera twice).
 *
 * Correct model, confirmed by buffer_probe_g3.js v4:
 *   - arg2 of AddInput/AddOutput is a DarwinnApi2_Buffer* handle, NOT a
 *     raw tensor pointer.
 *   - DarwinnApi2_Buffer_SizeBytes(handle) -> uint64, safe direct call.
 *   - DarwinnApi2_Buffer_MapToHost(handle, &outPtr) -> int32 status,
 *     writes the real host-mapped tensor pointer through outPtr. Confirmed
 *     working only when called DEFERRED (setTimeout(0)) outside the
 *     Interceptor onLeave stack — calling it synchronously inside onLeave
 *     is a documented Frida re-entrancy hazard (Pixel7_Build_Record.md:885)
 *     and crashed the camera even with zero writes.
 *
 * This script: after a 3-frame warmup, on the next Submit it captures the
 * current input slot 0 handle, defers via setTimeout(0), maps it to host,
 * writes a known 16-byte pattern (0x80 repeated, well within the confirmed
 * 150528-byte real allocation), flushes the cache for that buffer, then
 * watches subsequent output buffers for a change versus their pre-inject
 * baseline values — the G3 equivalent of G2's Phase 15c bidirectional proof.
 */

const LIB = 'libedgetpu_util.so';
const mod = Process.getModuleByName(LIB);

function resolve(name) {
    const addr = mod.getExportByName(name);
    console.log(`[resolve] ${name} -> ${addr}`);
    return addr;
}

const addInputAddr = resolve('DarwinnApi2_InferenceRequest_AddInput');
const addOutputAddr = resolve('DarwinnApi2_InferenceRequest_AddOutput');
const submitAddr = resolve('DarwinnApi2_VirtualDevice_Submit');
const sizeBytesAddr = resolve('DarwinnApi2_Buffer_SizeBytes');
const mapToHostAddr = resolve('DarwinnApi2_Buffer_MapToHost');
const flushCacheAddr = resolve('DarwinnApi2_Buffer_FlushCache');

const fnSizeBytes = new NativeFunction(sizeBytesAddr, 'uint64', ['pointer']);
const fnMapToHost = new NativeFunction(mapToHostAddr, 'int32', ['pointer', 'pointer']);
const fnFlushCache = new NativeFunction(flushCacheAddr, 'int32', ['pointer']);

const INJECT_VALUE = 0x80;
const WARMUP_FRAMES = 3;

let inputs = {};
let outputs = {};
let outputBaseline = {}; // slot -> last seen first byte
let frameCount = 0;
let injectScheduled = false;
let injected = false;
const startTime = Date.now();

Interceptor.attach(addInputAddr, {
    onEnter(args) {
        const slot = args[1].toInt32();
        inputs[slot] = args[2];
    }
});

Interceptor.attach(addOutputAddr, {
    onEnter(args) {
        const slot = args[1].toInt32();
        outputs[slot] = args[2];
    }
});

function readOutputsSnapshot() {
    const snap = {};
    for (const slot of Object.keys(outputs)) {
        const handle = outputs[slot];
        setTimeout(() => {
            try {
                const outPtrSlot = Memory.alloc(8);
                outPtrSlot.writePointer(NULL);
                fnMapToHost(handle, outPtrSlot);
                const hostPtr = outPtrSlot.readPointer();
                if (!hostPtr.isNull()) {
                    const sz = Math.min(16, fnSizeBytes(handle).toNumber());
                    const bytes = hostPtr.readByteArray(sz);
                    const hex = Array.from(new Uint8Array(bytes)).map(b => b.toString(16).padStart(2, '0')).join(' ');
                    const tag = injected ? 'POST-INJECT' : 'baseline';
                    console.log(`[OUT] frame=${frameCount} slot=${slot} (${tag}) = ${hex}`);
                }
            } catch (e) {
                console.log(`[OUT] frame=${frameCount} slot=${slot} ERROR: ${e.message}`);
            }
        }, 0);
    }
    return snap;
}

Interceptor.attach(submitAddr, {
    onEnter(args) { this.t0 = Date.now(); },
    onLeave(retval) {
        frameCount++;
        const elapsed = ((Date.now() - startTime) / 1000).toFixed(1);

        readOutputsSnapshot();

        if (!injectScheduled && frameCount === WARMUP_FRAMES && Object.keys(inputs).length > 0) {
            injectScheduled = true;
            const slot = Object.keys(inputs)[0];
            const handle = inputs[slot];
            setTimeout(() => {
                try {
                    const outPtrSlot = Memory.alloc(8);
                    outPtrSlot.writePointer(NULL);
                    fnMapToHost(handle, outPtrSlot);
                    const hostPtr = outPtrSlot.readPointer();
                    const sz = fnSizeBytes(handle).toNumber();
                    console.log(`[INJECT] slot=${slot} handle=${handle} hostPtr=${hostPtr} size=${sz}`);
                    if (!hostPtr.isNull()) {
                        hostPtr.writeByteArray(Array(16).fill(INJECT_VALUE));
                        const status = fnFlushCache(handle);
                        console.log(`[INJECT] wrote 16 bytes of 0x${INJECT_VALUE.toString(16)} at ${hostPtr}, FlushCache status=${status}`);
                        injected = true;
                    } else {
                        console.log('[INJECT] hostPtr was null, aborting injection');
                    }
                } catch (e) {
                    console.log(`[INJECT] FAILED: ${e.message}`);
                }
            }, 0);
        }

        console.log(`[frame ${frameCount}] t=${elapsed}s in_slots=${Object.keys(inputs).length} out_slots=${Object.keys(outputs).length} injected=${injected}`);
        inputs = {};
        outputs = {};
    }
});

console.log('[+] G3 bidirectional injection v3 (Buffer API, deferred) installed — waiting for frames...');
