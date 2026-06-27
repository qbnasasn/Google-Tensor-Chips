/*
 * Tensor G3 (Rio) DarWINN live-output proof.
 * Direct port of tpu_proof.js (G2/Janeiro) but using named dynamic exports
 * instead of hardcoded offsets, since libedgetpu_util.so on G3 is unstripped.
 * Frida 17.x API: Module.getExportByName (findExportByName was removed).
 */

const LIB = 'libedgetpu_util.so';
const mod = Process.getModuleByName(LIB);

function resolve(name) {
    const addr = mod.getExportByName(name);
    console.log(`[resolve] ${name} -> ${addr}`);
    return addr;
}

const addOutputAddr = resolve('DarwinnApi2_InferenceRequest_AddOutput');
const submitAddr = resolve('DarwinnApi2_VirtualDevice_Submit');

// arg0 = InferenceRequest*, arg1 = output slot index, arg2 = output buffer ptr, arg3 = 0
let outputs = {};
let frameCount = 0;
const startTime = Date.now();

Interceptor.attach(addOutputAddr, {
    onEnter(args) {
        const slot = args[1].toInt32();
        outputs[slot] = args[2];
    }
});

Interceptor.attach(submitAddr, {
    onEnter(args) {
        this.t0 = Date.now();
    },
    onLeave(retval) {
        const submitMs = Date.now() - this.t0;
        frameCount++;
        const elapsed = ((Date.now() - startTime) / 1000).toFixed(1);
        let parts = [];
        for (const slot of Object.keys(outputs)) {
            const ptr = outputs[slot];
            try {
                const bytes = ptr.readByteArray(16);
                parts.push(`slot${slot}=${hexdump(bytes, { length: 16, ansi: false }).split('\n')[0].split('  ')[1]}`);
            } catch (e) {
                parts.push(`slot${slot}=unreadable`);
            }
        }
        console.log(`[frame ${frameCount}] t=${elapsed}s submit_ms=${submitMs} ret=${retval} ${parts.join(' | ')}`);
        outputs = {};
    }
});

console.log('[+] Hooks installed on G3 libedgetpu_util.so — waiting for camera frames...');
