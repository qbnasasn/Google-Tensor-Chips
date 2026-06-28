/*
 * Tensor G3 (Rio) DarWINN bidirectional injection proof v4 — full-frame gray flip.
 *
 * v3 (inject_v3_g3.js) proved write capability but only wrote 16 bytes into the
 * corner of a 150,528-byte input tensor — too small/localized for a CNN to
 * register, so no causal effect was visible in the outputs (confirmed: outputs
 * stayed in the same numeric range before/after on a static scene).
 *
 * This version replicates G2's actual gray-flip demo (Pixel7_Build_Record.md
 * Phase 15c): write 0x80 across the FULL input tensor (all 150,528 bytes),
 * not just a few bytes, so the effect on the model's outputs is unambiguous.
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
    for (const slot of Object.keys(outputs)) {
        const handle = outputs[slot];
        setTimeout(() => {
            try {
                const outPtrSlot = Memory.alloc(8);
                outPtrSlot.writePointer(NULL);
                fnMapToHost(handle, outPtrSlot);
                const hostPtr = outPtrSlot.readPointer();
                if (!hostPtr.isNull()) {
                    const sz = Math.min(32, fnSizeBytes(handle).toNumber());
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
}

Interceptor.attach(submitAddr, {
    onEnter(args) { this.t0 = Date.now(); },
    onLeave(retval) {
        frameCount++;
        const elapsed = ((Date.now() - startTime) / 1000).toFixed(1);

        readOutputsSnapshot();

        if (frameCount >= WARMUP_FRAMES && Object.keys(inputs).length > 0) {
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
                        // Fill the FULL tensor with 0x80, not just a few bytes — matches G2's gray-flip demo.
                        hostPtr.writeByteArray(new Array(sz).fill(INJECT_VALUE));
                        const status = fnFlushCache(handle);
                        console.log(`[INJECT] wrote ${sz} bytes of 0x${INJECT_VALUE.toString(16)} (full tensor) at ${hostPtr}, FlushCache status=${status}`);
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

console.log('[+] G3 full-frame gray-flip injection v4 installed — waiting for frames...');
