# DarWINN SDK — API Reference

**Target**: Pixel 7 (Tensor G2 / Janeiro NPU) · Android 14  
**Process**: `com.google.android.GoogleCamera` · Portrait mode required  
**Library**: `libedgetpu_util.so` (all symbols resolved by name — APK-version-independent)

---

## Setup

**Requires**: Frida 17.x on host · `frida-server` running on device · Portrait mode active in GCamera.

`libedgetpu_util.so` loads only when Portrait mode is active. With a cold spawn you must trigger Portrait mode via ADB after resuming — the SDK's internal timer polls for the library automatically.

```python
import frida, subprocess, time, os

ADB = os.path.join(os.environ['LOCALAPPDATA'], r'Android\Sdk\platform-tools\adb.exe')

device  = frida.get_usb_device()
pid     = device.spawn(['com.google.android.GoogleCamera'])
session = device.attach(pid)
script  = session.create_script(open('darwinn_sdk.js').read())
script.load()
device.resume(pid)

# Trigger Portrait mode so libedgetpu_util.so loads (cold spawn only)
time.sleep(4)
subprocess.run([ADB, 'shell', 'input', 'swipe', '800', '2200', '200', '2200'])
time.sleep(0.5)
subprocess.run([ADB, 'shell', 'input', 'swipe', '800', '2200', '200', '2200'])
time.sleep(2)   # library loads ~1s after swipe; wait for _waitTimer to fire

api = script.exports
```

---

## Methods

### `darwinn_attach()` → dict

Resolves all DarWINN symbols and installs hooks. Must be called first.

```python
r = api.darwinn_attach()
# r = { ok: True, lib_base: "0x...", lib_size: 3964928,
#        symbols: { AddInput: "0x...", AddOutput: "0x...", Submit: "0x...", RG6: "0x..." } }
```

- Returns `ok: False` if `libedgetpu_util.so` is not yet loaded (Portrait mode not active).
- Safe to call multiple times — no-op after first attach.

---

### `darwinn_read_tensor(frame_index)` → dict

Returns a captured inference frame.

| Argument | Values |
|---|---|
| `frame_index` | `0` = oldest, `-1` = latest, `'all'` = full log (up to 64 frames) |

```python
r = api.darwinn_read_tensor(-1)
# r = { ok: True, frame: {
#   n: 42, ts: 1715000000123, submitMs: 62,
#   inputPtr:  "0xb4...",  inputHex:  "80 80 80 80 80 80 80 80",
#   outputPtr: "0xb4...",  outputHex: "28 60 3a 8b 7b 00 00 00 ..."
# }}
```

---

### `darwinn_write_tensor(data_hex)` → dict

Injects bytes into the input tensor on the next inference frame.

| Argument | Description |
|---|---|
| `data_hex` | Hex string of up to 150,528 bytes (224×224×3). Remainder filled with `0x80`. Pass `""` to inject solid gray. |

```python
# Inject solid gray (128 / 0x80) across full 224×224×3 input
r = api.darwinn_write_tensor("")
# r = { ok: True, queued_bytes: 150528 }

# Verify output flipped (~1 frame later):
time.sleep(0.1)
out = api.darwinn_read_tensor(-1)
print(out['frame']['outputHex'])   # expect: "80 80 80 80 ..." after injection
```

Injection is one-shot — fires on the next `AddInput` call only.

---

### `darwinn_get_pipeline_info()` → dict

Returns model metadata and live performance stats.

```python
r = api.darwinn_get_pipeline_info()
# r['info'] = {
#   process:      "com.google.android.GoogleCamera",
#   library:      "libedgetpu_util.so",
#   model:        "portrait/relight (HDRNet global-coefficients branch)",
#   tensor_name:  "inference/coefficients/global/conv2/biases",
#   input_shape:  [1, 224, 224, 3],   input_dtype: "uint8",   input_bytes: 150528,
#   output_shape: [288],              output_dtype: "uint8",
#   output_scale: "1/256",
#   frames_logged: 42,
#   avg_submit_ms: "62.3",
#   infer_hz:      "16.1"
# }
```

---

### `darwinn_flush_frames()` → dict

Clears the 64-frame rolling log.

---

## Quick-start example

```python
import frida, subprocess, time, os

ADB = os.path.join(os.environ['LOCALAPPDATA'], r'Android\Sdk\platform-tools\adb.exe')

device  = frida.get_usb_device()
pid     = device.spawn(['com.google.android.GoogleCamera'])
session = device.attach(pid)
script  = session.create_script(open('darwinn_sdk.js').read())

frames = []
def on_msg(msg, _):
    p = msg.get('payload', {})
    if p.get('t') == 'frame':
        frames.append(p['frame'])
    elif p.get('t') in ('ready', 'error'):
        print(f"[SDK] {p.get('msg', p)}")

script.on('message', on_msg)
script.load()
device.resume(pid)

# Cold spawn: trigger Portrait mode so libedgetpu_util.so loads
time.sleep(4)
subprocess.run([ADB, 'shell', 'input', 'swipe', '800', '2200', '200', '2200'])
time.sleep(0.5)
subprocess.run([ADB, 'shell', 'input', 'swipe', '800', '2200', '200', '2200'])
time.sleep(2)   # wait for _waitTimer to detect library

api = script.exports
r = api.darwinn_attach()
if not r['ok']:
    raise RuntimeError(f"attach failed: {r}")
print(f"Attached — lib_base={r['lib_base']}")

time.sleep(2)           # collect baseline frames
print(api.darwinn_get_pipeline_info())

api.darwinn_write_tensor("")    # inject solid gray (0x80)
time.sleep(0.2)
print(api.darwinn_read_tensor(-1))   # confirm output flip

session.detach()
```

---

## Tensor reference

| Field | Value |
|---|---|
| Input tensor | 224 × 224 × 3 · uint8 · layout NHWC |
| Output tensor | 288 bytes · uint8 · scale 1/256 |
| Inference rate | ~16 Hz (Submit is async; hardware runs independently) |
| Pointer chain | `AddInput.args[0][+136][+80][+16]` → data pointer |
| Output chain | `AddOutput.args[2]` → output buffer (MTE-tagged, 0xb4...) |

---

## Notes

- **Portrait mode must be active** before calling `darwinn_attach()`. If the library is not loaded, the method returns `ok: False`.  
- All symbols are resolved by **named export** from `libedgetpu_util.so` — no hardcoded offsets, works across GCamera APK versions.  
- `VirtualDevice_Submit` is non-blocking (async queue). Output is readable ~1 frame (62ms) after injection.  
- MTE (Memory Tagging Extension) is active on Android 14. Do not allocate separate buffers for injection — write into the pointer captured from `AddInput` directly (as this SDK does).
