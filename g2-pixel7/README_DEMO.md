# DarWINN NPU Access — Proof of Capability

**Device**: Google Pixel 7 · Tensor G2 SoC · Janeiro NPU (DarWINN architecture)  
**Demonstrated**: May 2026 · Phase 15c

---

## What was proven

A custom tool running on a connected PC gained **full bidirectional read/write access** to the Tensor G2 NPU while it was actively running Google Camera's portrait mode inference pipeline.

Specifically:

1. **Read**: The tool intercepted the live inference loop (running at ~16 frames/sec) and read the raw bytes of both the input tensor and output tensor on every frame.

2. **Write**: The tool replaced the input tensor data with a solid gray image (every pixel set to RGB 128, 128, 128) mid-pipeline.

3. **Confirmed**: The NPU processed the injected data and wrote a corresponding output — the output tensor shifted to `0x80` across all bytes, proving the hardware received and acted on the injected input.

This was done **without modifying any app, firmware, or model file**. The running Google Camera process was untouched from its own perspective.

---

## Why this is significant

### The NPU is normally locked

The Tensor G2 NPU is a proprietary Google silicon block (DarWINN / Janeiro). It has no public API. Third-party apps cannot access it directly — only Google's own Camera app and signed system services are permitted to register and run models on the hardware.

### What was bypassed

By attaching to the running Google Camera process and hooking three internal DarWINN API calls (`AddInput`, `AddOutput`, `VirtualDevice_Submit`), the tool operates at the same privilege level as the Camera app itself — inside an authorized process, with full access to the NPU's live tensor buffers.

### The pointer chain

The NPU driver does not expose tensor data through a simple pointer. Reaching the actual pixel buffer requires following a three-level chain of heap pointers inside the DarWINN request object:

```
InferenceRequest object
  └─ [+136] → Level 1 object (MTE-tagged heap pointer)
      └─ [+80]  → Inner object
          └─ [+16] → Tensor data buffer (224×224×3 = 150,528 bytes)
```

This chain was reverse-engineered from live memory during 270+ inference frames. It is stable across camera sessions (chain offsets do not change with ASLR).

---

## The demo result

**Before injection** (frames 1–3): output tensor contains live, camera-dependent coefficient data — structured non-zero values that change as the scene changes.

**After injection** (frame 4+): the tool wrote `0x80` (decimal 128) to all 150,528 input bytes. The output tensor changed to `0x80` across all 288 bytes.

```
[FRAME 03]  input=28 3c 41 ...  output=28 60 3a 8b 7b 00 ...  (live scene)
[FRAME 04]  input=80 80 80 ...  output=80 80 80 80 80 80 ...  (injected gray)
```

The flip is the proof: the NPU ran a full inference pass on injected data and returned a result derived from that data. End-to-end hardware control confirmed.

---

## What the SDK provides

The included `darwinn_sdk.js` packages this capability into four clean calls:

| Method | What it does |
|---|---|
| `darwinn_attach()` | Connect to the live NPU pipeline in Google Camera |
| `darwinn_read_tensor(n)` | Read input or output tensor bytes from any captured frame |
| `darwinn_write_tensor(data)` | Inject custom bytes into the next inference frame |
| `darwinn_get_pipeline_info()` | Report model name, tensor shape, latency, and frame rate |

All DarWINN function addresses are resolved at runtime by named export — the SDK works across Google Camera APK versions without re-pinning offsets.

---

## Technical environment

| Property | Value |
|---|---|
| Device | Google Pixel 7 |
| SoC | Google Tensor G2 (Samsung S5300B) |
| NPU | DarWINN / Janeiro (Google proprietary) |
| Android version | 14 |
| Root | Magisk (init_boot patch) |
| Hook framework | Frida 17.9.5 |
| Target process | com.google.android.GoogleCamera |
| NPU library | libedgetpu_util.so (DarWINN API v2) |
| Inference rate | ~16 Hz (async hardware queue) |
| Input tensor | 224 × 224 × 3 uint8 (150,528 bytes) |
| Output tensor | 288 bytes uint8 (global HDRNet coefficients) |

---

## Additional capability mapped (not included in base SDK)

The following was also reverse-engineered during the research phase and is available as an add-on:

- **Custom model execution** (`CUSTOM_MODEL_EXECUTION.md`): networks we author — not Google's — compile to and run on the locked NPU via the `google-edgetpu` accelerator. Proven path + measured performance envelope (strong for vision/CNN; bandwidth-bound for LLM decode).
- **Stage-2 compiler pipeline**: `CompileTfliteFlatbuffer2` in `vendor.google.edgetpu_vendor_service` — full argument layout mapped, function hooked and confirmed working. Enables interception of the device-specific DGC0 compilation step.
- **DGC0 cache format**: the 59-byte outer wrapper, inner FlatBuffer layout, and checksum format are fully documented. DGC0 binaries can be extracted and analysed.
- **RegisterGraph6 format**: the exact input format (4-byte length prefix + DGC0 FlatBuffer) required to register a compiled graph on the live DarWINN VirtualDevice.
