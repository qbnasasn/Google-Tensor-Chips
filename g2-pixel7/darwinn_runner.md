# DarWINN Standalone NPU Runner — Build & Deploy

**Target**: Pixel 7 · Tensor G2 / Janeiro NPU · Android 14 · Magisk root  
**ABI**: arm64-v8a · NDK API 30+  
**Source**: `darwinn_runner.cpp`  
**No Frida. No GCamera dependency. Root required.**

---

## Prerequisites

| Requirement | Version / Path |
|---|---|
| Android NDK | 27.x at `%LOCALAPPDATA%\Android\Sdk\ndk\27.1.12297006` |
| ADB | `%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe` |
| Device | Pixel 7 · Magisk root · USB debugging enabled |
| frida-server | Not required for this binary |

---

## Build (Windows — cross-compile for arm64-v8a)

```powershell
$NDK = "$env:LOCALAPPDATA\Android\Sdk\ndk\27.1.12297006"
$CC  = "$NDK\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android30-clang++.cmd"

& $CC `
  -std=c++17 `
  -O2 -fPIE -pie `
  -target aarch64-linux-android30 `
  C:\Pixel7\broker\darwinn_runner.cpp `
  -o C:\Pixel7\bin\darwinn_runner `
  -landroid `
  -ldl

# Verify
if ($?) { Write-Host "Build OK" } else { Write-Host "Build FAILED" }
```

**Required link flags:**
- `-landroid` — AHardwareBuffer API
- `-ldl` — dlopen / dlsym

**Note**: `aarch64-linux-android30` targets API 30 (Android 11+). AHardwareBuffer is available from API 26; targeting 30 gives access to `AHARDWAREBUFFER_FORMAT_BLOB` without deprecation warnings.

---

## Deploy

```powershell
$ADB = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"

& $ADB push C:\Pixel7\bin\darwinn_runner /data/local/tmp/darwinn_runner
& $ADB shell chmod 755 /data/local/tmp/darwinn_runner
```

---

## DGC0 Model — Source and Format

RegisterGraph6 expects a **raw DGC0 FlatBuffer** — not a TFLite wrapper and not a pre-prefixed buffer. The DGC0 file format (confirmed Phase 15e):

```
bytes [0–3]:  FlatBuffer root offset (little-endian)  e.g. d0 0f 00 00
bytes [4–7]:  "DGC0" magic                             = 44 47 43 30
bytes [8–N]:  FlatBuffer data
```

### Source 1 — edgetpu cache (easiest, already compiled for Tensor G2)

Populated automatically after any portrait mode camera session:

```powershell
# List cache files on device
$ADB = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
& $ADB shell "su -c 'ls -lh /data/vendor/edgetpu/cache/'"

# Pull one (replace <hash> with actual filename)
& $ADB pull /data/vendor/edgetpu/cache/<hash>  C:\Pixel7\bin\model.bin
```

Cache files have a **59-byte outer wrapper** prepended before the DGC0 content. The runner detects and strips this automatically by checking for DGC0 magic at offset 59.

### Source 2 — extract from TFLite (camera HAL models)

Portrait mode TFLite models in `/apex/com.google.pixel.camera.hal/` contain the compiled DGC0 subgraph in the `custom_options` field of the `edgetpu-custom-op-2` operator. Use the existing extraction script:

```powershell
# Pull a camera HAL model
& $ADB shell "su -c 'find /apex/com.google.pixel.camera.hal -name \"*.tflite\"'"
& $ADB pull /apex/com.google.pixel.camera.hal/<model>.tflite  C:\Pixel7\bin\

# Extract DGC0 subgraph (Phase 15 script)
python C:\Pixel7\scripts\extract_edgetpu_graph.py `
    C:\Pixel7\bin\<model>.tflite `
    C:\Pixel7\bin\model.dgc0
```

Push the extracted DGC0 to device:
```powershell
& $ADB push C:\Pixel7\bin\model.dgc0 /data/local/tmp/model.dgc0
```

---

## SELinux — Critical Context Requirement

**Phase 15 confirmed**: `CreateVirtualDevice` called from `u:r:shell:s0` or `u:r:su:s0` returns a VirtualDevice with a **null vtable** — Binder IPC connections to the edgetpu vendor service are not initialized in those contexts. Subsequent `RegisterGraph6` or `CreateInferenceRequest` calls will SIGABRT.

### Required SELinux context

```bash
# Preferred: hal_camera_default has edgetpu binder access
adb shell "su -c 'runcon u:r:hal_camera_default:s0 /data/local/tmp/darwinn_runner --model /data/local/tmp/model.bin'"

# Alternative: vendor edgetpu service context
adb shell "su -c 'runcon u:r:vendor_edgetpu_service:s0 /data/local/tmp/darwinn_runner --model /data/local/tmp/model.bin'"
```

### Permissive mode (testing only)

```bash
adb shell "su -c 'setenforce 0'"
adb shell "su -c '/data/local/tmp/darwinn_runner --model /data/local/tmp/model.bin'"
# Restore when done:
adb shell "su -c 'setenforce 1'"
```

### Diagnosing context issues

```bash
# Check available edgetpu-related SELinux types
adb shell "su -c 'cat /vendor/etc/selinux/vendor_sepolicy.cil | grep edgetpu | grep type'"

# Confirm vtable status in runner output:
# "[darwinn] VirtualDevice vtable @ 0x..."   ← initialized (correct context)
# "[darwinn] *** VTABLE NULL ***"            ← wrong context, use runcon
```

---

## Run

```bash
# Full launch sequence:
adb shell "su -c 'runcon u:r:hal_camera_default:s0 \
  /data/local/tmp/darwinn_runner \
  --model /data/local/tmp/model.bin \
  --socket /data/local/tmp/darwinn.sock'"
```

Expected startup output (correct context):
```
[darwinn] loaded /vendor/lib64/libedgetpu_util.so
[darwinn] GetVersionInfo: ret=1 version=7
[darwinn] DeviceSpec: type=3 chip=1 p2=0 p3=1 cfg=0x... p5=1
[darwinn] VirtualDevice @ 0xb4...
[darwinn] VirtualDevice vtable @ 0x...  (initialized OK)
[darwinn] DGC0 loaded: 327810 bytes  root_offset=0x0fd0
[darwinn] Graph registered @ 0xb4...
[darwinn] input_tensor=0x...  output_tensor=0x...
[darwinn] tensor buffers allocated
[darwinn] ready — accepting inference requests
[server] listening on /data/local/tmp/darwinn.sock
```

---

## Python Client

### On-device (Termux or adb shell)

```python
import socket, struct

def run_inference(input_bytes, sock='/data/local/tmp/darwinn.sock'):
    """
    Send uint8 tensor bytes, receive output bytes + latency.
    input_bytes: bytes of length <= 150528 (224x224x3)
    Returns: (output_bytes: bytes[288], latency_us: int)
    """
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(sock)
        s.sendall(struct.pack('<I', len(input_bytes)))
        s.sendall(input_bytes)

        status,   = struct.unpack('<i', _recv_exact(s, 4))
        out_size, = struct.unpack('<I', _recv_exact(s, 4))
        output    = _recv_exact(s, out_size)
        lat_us,   = struct.unpack('<q', _recv_exact(s, 8))

    if status != 0:
        raise RuntimeError(f'inference failed: status={status}')
    return output, lat_us

def _recv_exact(s, n):
    buf = b''
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk: raise ConnectionError('connection closed')
        buf += chunk
    return buf

# ── Quick test: inject solid gray (0x80), expect output to flip ──────────────
input_data = bytes([0x80] * 150528)
output, lat = run_inference(input_data)

print(f'latency:    {lat / 1000:.1f} ms')
print(f'output[0:8]: {output[:8].hex()}')
print(f'output mean: {sum(output)/len(output):.1f}  (expect ~128 for gray input)')
```

### From PC via ADB forward

```powershell
# Forward Unix socket to TCP port on PC
$ADB = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
& $ADB forward tcp:7999 local:/data/local/tmp/darwinn.sock
```

```python
import socket, struct

def run_inference_tcp(input_bytes, host='127.0.0.1', port=7999):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((host, port))
        s.sendall(struct.pack('<I', len(input_bytes)))
        s.sendall(input_bytes)

        status,   = struct.unpack('<i', _recv_exact(s, 4))
        out_size, = struct.unpack('<I', _recv_exact(s, 4))
        output    = _recv_exact(s, out_size)
        lat_us,   = struct.unpack('<q', _recv_exact(s, 8))

    if status != 0:
        raise RuntimeError(f'inference failed: status={status}')
    return output, lat_us
```

---

## Socket Protocol Reference

```
Client → Server:
  [4 bytes] uint32_le  input_size  (max 150528)
  [N bytes] uint8[]    input tensor (224×224×3 NHWC uint8)

Server → Client:
  [4 bytes] int32_le   status      (0 = OK, negative = error)
  [4 bytes] uint32_le  output_size (288 on success, 0 on error)
  [N bytes] uint8[]    output tensor (288-byte HDRNet coefficients, scale=1/256)
  [8 bytes] int64_le   latency_us  (Submit call to output buffer read)
```

One request per connection. Server loops accepting new connections.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `dlopen failed: permission denied` | SELinux blocks library access from shell | `runcon u:r:hal_camera_default:s0 ...` |
| `VirtualDevice vtable @ 0x0` | Wrong SELinux context, Binder not initialized | `runcon` or `setenforce 0` |
| `RegisterGraph6 returned null` | Null vtable vdev or wrong DGC0 format | Fix vtable first; validate DGC0 magic bytes 4-7 = `44 47 43 30` |
| `DGC0 magic not found` | Passing TFLite wrapper or unparsed cache file | Use extract_edgetpu_graph.py; cache files auto-stripped |
| `CreateInferenceRequest returned null` | Vtable null, or 3-arg form needs callback | Fix vtable; try `CreateInferenceRequest3` if available |
| `AddInput error: N` or SIGABRT at AddInput | Signature mismatch ([INFERRED]) | Disassemble AddInput; count args and adjust typedef |
| `Submit callback not received` | Submit may not accept callback in this form | Ignored — runner falls back to 500ms timed wait |
| Output all-zeros | NPU completed but AHardwareBuffer vendor bit needed | Add `d.usage \|= (1ULL << 28)` in `ahb_alloc()` |

---

## Discover All Available Exports

If symbol resolution fails or you need alternate variants:

```bash
# On device (requires readelf from NDK or busybox)
adb shell "su -c 'readelf -sW /vendor/lib64/libedgetpu_util.so | grep DarwinnApi2'"
adb shell "su -c 'readelf -sW /vendor/lib64/libedgetpu_util.so | grep DarwinnDelegate'"
```

Or from PC using NDK's readelf:

```powershell
$NDK = "$env:LOCALAPPDATA\Android\Sdk\ndk\27.1.12297006"
& "$NDK\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe" `
  -sW C:\Pixel7\bin\libedgetpu_util.so | Select-String "DarwinnApi2"
```

Known variant families (Phase 15 export scan):
```
RegisterGraph variants:   RegisterGraph, RegisterGraph3–6, RegisterGraphByBuffer3–6
InferenceRequest variants: CreateInferenceRequest, CreateInferenceRequest3
Execution:                RequestQueue_Submit, VirtualDevice_Submit, VirtualDevice_SubmitFenced
Completion:               Request_Wait, Request_IsCompleted, Request_GetFuture, Request_GetTiming
```

---

## Tensor Reference

| Field | Value | Source |
|---|---|---|
| Input tensor | 224 × 224 × 3 · uint8 · NHWC | Phase 15c confirmed |
| Output tensor | 288 bytes · uint8 · scale 1/256 | Phase 15d confirmed |
| Model | Portrait HDRNet relight (global-coefficients branch) | Phase 15d |
| Tensor name | `inference/coefficients/global/conv2/biases` | Phase 15d |
| Inference rate | ~16 Hz (Submit async queue, 62ms hardware) | Phase 15b |
| Pointer chain (Frida) | `AddInput.args[0][+136][+80][+16]` → data | Phase 15c |
| DGC0 magic | `d0 0f 00 00 44 47 43 30` at offset 0 of compiled graph | Phase 15e |
