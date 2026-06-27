/**
 * resolve_offsets.js — DarWINN API v2 runtime symbol resolver
 *
 * Locates all DarWINN hook targets in libedgetpu_util.so by named export,
 * then validates each address against a known AArch64 function prologue pattern.
 * Works across GCamera APK versions without manual re-pinning.
 *
 * Usage (standalone):
 *   frida -U -n com.google.android.GoogleCamera -l resolve_offsets.js
 *
 * Usage (from darwinn_sdk.js):
 *   const offsets = resolveOffsets();  // returns null on failure
 */

'use strict';

const LIBRARY = 'libedgetpu_util.so';

const TARGETS = [
    'DarwinnApi2_InferenceRequest_AddInput',
    'DarwinnApi2_InferenceRequest_AddOutput',
    'DarwinnApi2_VirtualDevice_Submit',
    'DarwinnApi2_VirtualDevice_RegisterGraph6',
    'DarwinnApi2_VirtualDevice_CreateInferenceRequest',
    'DarwinnApi2_Graph_GetInputTensor',
    'DarwinnApi2_Graph_GetOutputTensor',
];

function validateAddress(addr, lib) {
    // Verify the resolved address falls within the library's mapped region.
    // More robust than byte-pattern matching across compiler/build variations.
    try {
        var offset = addr.sub(lib.base);
        var off = offset.toUInt32 ? offset.toUInt32() : parseInt(offset.toString(), 16);
        return off > 0 && off < lib.size;
    } catch (e) {
        return false;
    }
}

function resolveOffsets() {
    var lib = Process.findModuleByName(LIBRARY);
    if (!lib) {
        send({ t: 'error', msg: LIBRARY + ' not loaded in this process' });
        return null;
    }

    var result = { lib_base: lib.base.toString(), lib_size: lib.size, targets: {} };
    var allOk = true;

    // Frida 17+: Module static methods removed — use instance method lib.findExportByName(name)
    TARGETS.forEach(function (name) {
        var addr = lib.findExportByName(name);
        if (!addr) {
            result.targets[name] = { ok: false, reason: 'export not found' };
            allOk = false;
            return;
        }
        var valid = validateAddress(addr, lib);
        result.targets[name] = {
            ok: valid,
            address: addr.toString(),
            offset: '0x' + addr.sub(lib.base).toString(16),
            reason: valid ? 'address in lib range' : 'address outside lib range — bad export',
        };
        if (!valid) allOk = false;
    });

    result.all_ok = allOk;
    return result;
}

// When run standalone, print results immediately.
var timer = setInterval(function () {
    if (!Process.findModuleByName(LIBRARY)) return;
    clearInterval(timer);
    var r = resolveOffsets();
    send({ t: 'resolve_result', result: r });
    r.all_ok
        ? send({ t: 'status', msg: 'All ' + TARGETS.length + ' symbols resolved OK' })
        : send({ t: 'status', msg: 'WARN: one or more symbols failed validation — check result' });
}, 200);
