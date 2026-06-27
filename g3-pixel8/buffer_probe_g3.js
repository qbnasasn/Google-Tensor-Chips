/*
 * Tensor G3 (Rio) DarWINN Buffer-handle probe v4 — DEFERRED, READ ONLY.
 *
 * v3 passively observed the camera's own MapToHost calls and confirmed the
 * real signature: MapToHost(handle, &outPtr) — arg1 is a caller-supplied
 * stack slot the function writes the host pointer into, NOT a simple
 * pointer-return. retval was consistently 0x0 (likely an OK status code).
 *
 * v1/v2 crashed GoogleCamera by calling MapToHost ourselves synchronously
 * from inside Submit's onLeave — a documented Frida re-entrancy hazard
 * (see Pixel7_Build_Record.md:885, same issue hit on G2's RegisterGraph6).
 * Fix (per that doc): defer the call with setTimeout(fn, 0) to exit the
 * interceptor stack first. Unlike G2's case, nothing frees our buffer in
 * the meantime, so this should be safe. Still READ ONLY — no writes.
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

const fnSizeBytes = new NativeFunction(sizeBytesAddr, 'uint64', ['pointer']);
// Confirmed signature from passive observation: (handle, &outPtr), status-like retval.
const fnMapToHost = new NativeFunction(mapToHostAddr, 'int32', ['pointer', 'pointer']);

let inputs = {};
let outputs = {};
let frameCount = 0;
let probeScheduled = false;
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

function deferredProbe(label, slot, handle) {
    setTimeout(() => {
        try {
            const sz = fnSizeBytes(handle);
            const outPtrSlot = Memory.alloc(8);
            outPtrSlot.writePointer(NULL);
            const status = fnMapToHost(handle, outPtrSlot);
            const hostPtr = outPtrSlot.readPointer();
            console.log(`[DEFERRED] ${label} slot=${slot} handle=${handle} SizeBytes=${sz} MapToHost_status=${status} hostPtr=${hostPtr}`);
            if (!hostPtr.isNull()) {
                const n = Math.min(32, sz.toNumber ? sz.toNumber() : 32);
                const bytes = hostPtr.readByteArray(n);
                console.log(hexdump(bytes, { length: n, ansi: false }));
            }
        } catch (e) {
            console.log(`[DEFERRED] ${label} slot=${slot} handle=${handle} ERROR: ${e.message}`);
        }
    }, 0);
}

Interceptor.attach(submitAddr, {
    onEnter(args) { this.t0 = Date.now(); },
    onLeave(retval) {
        frameCount++;
        const elapsed = ((Date.now() - startTime) / 1000).toFixed(1);

        if (!probeScheduled && frameCount === 2) {
            probeScheduled = true;
            for (const slot of Object.keys(inputs)) {
                deferredProbe('input', slot, inputs[slot]);
            }
            for (const slot of Object.keys(outputs)) {
                deferredProbe('output', slot, outputs[slot]);
            }
        }

        console.log(`[frame ${frameCount}] t=${elapsed}s in_slots=${Object.keys(inputs).length} out_slots=${Object.keys(outputs).length}`);
        inputs = {};
        outputs = {};
    }
});

console.log('[+] G3 Buffer probe v4 (deferred, read-only) installed...');
