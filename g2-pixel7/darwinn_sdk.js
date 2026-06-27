/**
 * darwinn_sdk.js — DarWINN NPU Access SDK
 * Target: Pixel 7 (Tensor G2 / Janeiro NPU), Android 14
 * Process: com.google.android.GoogleCamera (Portrait mode active)
 *
 * Proven capability (Phase 15c, May 2026):
 *   - Full bidirectional tensor read/write via DarWINN API v2
 *   - Live 16 Hz inference loop observable and injectable
 *   - Gray injection (0x80) → output flips confirmed on device
 *
 * Usage:
 *   frida -U -n com.google.android.GoogleCamera -l darwinn_sdk.js
 *   Then call API methods via RPC from host Python:
 *     session.exports.darwinn_attach()
 *     session.exports.darwinn_read_tensor(0)
 *     session.exports.darwinn_write_tensor(<bytes>)
 *     session.exports.darwinn_get_pipeline_info()
 */

'use strict';

// ── Internal state ──────────────────────────────────────────────────────────
var _attached   = false;
var _offsets    = null;
var _frameLog   = [];          // [{n, ts, inputPtr, outputPtr, outputHex, submitMs}]
var _injectNext = null;        // Uint8Array to write on the next AddInput call
var _hooks      = [];
var MAX_FRAMES  = 64;

// Pointer chain constants (confirmed empirical, Phase 15c)
var INPUT_CHAIN  = [136, 80, 16];   // args[0][+136][+80][+16] → data ptr
var INPUT_SIZE   = 150528;          // 224×224×3 uint8
var OUTPUT_SIZE  = 288;             // 288-byte uint8 coefficient vector

// ── Symbol resolution ───────────────────────────────────────────────────────
var LIBRARY = 'libedgetpu_util.so';
var SYM = {
    AddInput:  'DarwinnApi2_InferenceRequest_AddInput',
    AddOutput: 'DarwinnApi2_InferenceRequest_AddOutput',
    Submit:    'DarwinnApi2_VirtualDevice_Submit',
    RG6:       'DarwinnApi2_VirtualDevice_RegisterGraph6',
};

function _resolveSymbols() {
    var lib = Process.findModuleByName(LIBRARY);
    if (!lib) return null;
    var out = { base: lib.base, size: lib.size };
    // Frida 17+: Module static methods removed — use instance method lib.findExportByName(name)
    for (var key in SYM) {
        var addr = lib.findExportByName(SYM[key]);
        if (!addr) return null;
        out[key] = addr;
    }
    return out;
}

// ── Pointer chain helper ────────────────────────────────────────────────────
function _followChain(ptr, chain) {
    var cur = ptr;
    for (var i = 0; i < chain.length; i++) {
        try {
            cur = cur.add(chain[i]).readPointer();
            if (cur.isNull() || cur.compare(ptr('0xb400000000000000')) < 0) return null;
        } catch (e) { return null; }
    }
    return cur;
}

// ── Hook installation ───────────────────────────────────────────────────────
function _installHooks(sym) {
    var lastInputPtr  = null;
    var lastOutputPtr = null;
    var lastSubmitT   = 0;
    var frameN        = 0;

    // AddInput — capture input tensor pointer, optionally inject data
    _hooks.push(Interceptor.attach(sym.AddInput, {
        onEnter: function (args) {
            var dataPtr = _followChain(args[0], INPUT_CHAIN);
            if (!dataPtr) return;
            lastInputPtr = dataPtr;

            if (_injectNext) {
                var u8 = _injectNext;
                _injectNext = null;
                var chunk = 65536, off = 0;
                while (off < u8.length) {
                    var end = Math.min(off + chunk, u8.length);
                    dataPtr.add(off).writeByteArray(Array.prototype.slice.call(u8, off, end));
                    off = end;
                }
            }
        }
    }));

    // AddOutput — capture output buffer pointer
    _hooks.push(Interceptor.attach(sym.AddOutput, {
        onEnter: function (args) {
            try {
                var cand = args[2].readPointer();
                lastOutputPtr = (cand && !cand.isNull() &&
                    cand.compare(ptr('0xb400000000000000')) >= 0) ? cand : args[2];
            } catch (e) { lastOutputPtr = args[2]; }
        }
    }));

    // VirtualDevice_Submit — measure latency, capture output, log frame
    _hooks.push(Interceptor.attach(sym.Submit, {
        onEnter: function () { this._t0 = Date.now(); },
        onLeave: function () {
            frameN++;
            var now   = Date.now();
            var ms    = lastSubmitT ? (now - lastSubmitT) : 0;
            lastSubmitT = now;

            var inHex  = 'n/a';
            var outHex = 'n/a';

            try {
                if (lastInputPtr)
                    inHex = Array.from(new Uint8Array(lastInputPtr.readByteArray(8)))
                        .map(function (b) { return ('0' + b.toString(16)).slice(-2); }).join(' ');
            } catch (e) {}

            try {
                if (lastOutputPtr && !lastOutputPtr.isNull())
                    outHex = Array.from(new Uint8Array(lastOutputPtr.readByteArray(16)))
                        .map(function (b) { return ('0' + b.toString(16)).slice(-2); }).join(' ');
            } catch (e) {}

            var entry = {
                n:         frameN,
                ts:        now,
                submitMs:  ms,
                inputPtr:  lastInputPtr  ? lastInputPtr.toString()  : null,
                outputPtr: lastOutputPtr ? lastOutputPtr.toString() : null,
                inputHex:  inHex,
                outputHex: outHex,
            };
            if (_frameLog.length >= MAX_FRAMES) _frameLog.shift();
            _frameLog.push(entry);

            send({ t: 'frame', frame: entry });
        }
    }));
}

// ── Public API (exposed via Frida RPC) ─────────────────────────────────────

rpc.exports = {

    /**
     * darwinn_attach() → { ok, lib_base, lib_size, symbols }
     * Resolves DarWINN symbols and installs hooks.
     * Must be called before any other API method.
     * Requires Portrait mode to be active in GoogleCamera.
     */
    darwinn_attach: function () {
        if (_attached) return { ok: true, msg: 'already attached' };

        var sym = _resolveSymbols();
        if (!sym) return { ok: false, msg: LIBRARY + ' not loaded — is Portrait mode active?' };

        _offsets = sym;
        _installHooks(sym);
        _attached = true;

        return {
            ok:       true,
            lib_base: sym.base.toString(),
            lib_size: sym.size,
            symbols: {
                AddInput:  sym.AddInput.toString(),
                AddOutput: sym.AddOutput.toString(),
                Submit:    sym.Submit.toString(),
                RG6:       sym.RG6.toString(),
            }
        };
    },

    /**
     * darwinn_read_tensor(frame_index) → { ok, frame } | { ok, frames }
     * frame_index: integer (0 = oldest, -1 = latest) or 'all'
     * Returns captured frame data including input/output hex snapshots.
     */
    darwinn_read_tensor: function (frameIndex) {
        if (!_attached) return { ok: false, msg: 'call darwinn_attach() first' };
        if (_frameLog.length === 0) return { ok: false, msg: 'no frames captured yet' };

        if (frameIndex === 'all') return { ok: true, frames: _frameLog.slice() };

        var idx = (frameIndex < 0)
            ? Math.max(0, _frameLog.length + frameIndex)
            : Math.min(frameIndex, _frameLog.length - 1);

        return { ok: true, frame: _frameLog[idx] };
    },

    /**
     * darwinn_write_tensor(dataHex) → { ok, queued_bytes }
     * dataHex: hex string (e.g. "808080...") — must be 150528 bytes (224×224×3)
     *   OR pass a shorter string to fill the rest with 0x80.
     * Data is written on the NEXT AddInput call (next inference frame).
     * Confirm success by calling darwinn_read_tensor(-1) and checking outputHex flipped.
     */
    darwinn_write_tensor: function (dataHex) {
        if (!_attached) return { ok: false, msg: 'call darwinn_attach() first' };

        var bytes = new Uint8Array(INPUT_SIZE);
        bytes.fill(0x80);  // default fill

        if (dataHex && dataHex.length > 0) {
            var hexStr = dataHex.replace(/\s/g, '');
            var len = Math.min(hexStr.length / 2, INPUT_SIZE);
            for (var i = 0; i < len; i++) {
                bytes[i] = parseInt(hexStr.substr(i * 2, 2), 16);
            }
        }

        _injectNext = bytes;
        return { ok: true, queued_bytes: INPUT_SIZE };
    },

    /**
     * darwinn_get_pipeline_info() → { ok, info }
     * Returns model/tensor metadata and live performance stats.
     */
    darwinn_get_pipeline_info: function () {
        if (!_attached) return { ok: false, msg: 'call darwinn_attach() first' };

        var latencies = _frameLog.slice(-10).map(function (f) { return f.submitMs; }).filter(function (ms) { return ms > 0; });
        var avgMs = latencies.length
            ? (latencies.reduce(function (a, b) { return a + b; }, 0) / latencies.length).toFixed(1)
            : null;

        return {
            ok: true,
            info: {
                process:       'com.google.android.GoogleCamera',
                library:       LIBRARY,
                lib_base:      _offsets ? _offsets.base.toString() : null,
                model:         'portrait/relight (HDRNet global-coefficients branch)',
                tensor_name:   'inference/coefficients/global/conv2/biases',
                input_shape:   [1, 224, 224, 3],
                input_dtype:   'uint8',
                input_bytes:   INPUT_SIZE,
                output_shape:  [288],
                output_dtype:  'uint8',
                output_scale:  '1/256 (dequantize: value * 3.90625e-3)',
                frames_logged: _frameLog.length,
                avg_submit_ms: avgMs,
                infer_hz:      avgMs ? (1000 / parseFloat(avgMs)).toFixed(1) : null,
            }
        };
    },

    /**
     * darwinn_flush_frames() → clears the frame log
     */
    darwinn_flush_frames: function () {
        _frameLog = [];
        return { ok: true };
    },
};

// Notify host when library loads
var _waitTimer = setInterval(function () {
    if (!Process.findModuleByName(LIBRARY)) return;
    clearInterval(_waitTimer);
    send({ t: 'ready', msg: LIBRARY + ' loaded — call darwinn_attach() to begin' });
}, 200);
