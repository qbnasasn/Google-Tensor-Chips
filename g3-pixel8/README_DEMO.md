# DarWINN NPU Access — Tensor G3 (Pixel 8)

**Device**: Google Pixel 8 · Tensor G3 SoC · DarWINN NPU
**Demonstrated**: June 2026

---

## What was proven

Full bidirectional read/write access to the Tensor G3 NPU while it ran Google Camera's
live portrait-mode pipeline — the same capability as [`../g2-pixel7/README_DEMO.md`](../g2-pixel7/README_DEMO.md)
proved on the G2, reached via a different internal API.

1. **Read**: intercepted the live inference loop and read real tensor bytes from both
   input (150,528 bytes — a 224×224×3 camera frame) and output buffers (small scalar
   results, 1–288 bytes, varying by inference pass).
2. **Write**: wrote a known 16-byte pattern into the live input tensor mid-pipeline and
   flushed it to the hardware.
3. **Confirmed**: no crash, no corruption — both operations completed cleanly through the
   correct API surface (see below). Google Camera was otherwise untouched.

---

## Why G3 needed a different approach than G2

On the G2, `AddInput`/`AddOutput` hand back a **raw pointer** to the tensor bytes — you can
read/write it directly. On the G3, the same calls hand back an **opaque `DarwinnApi2_Buffer*`
handle**, not a raw pointer. Treating it like the G2's raw pointer corrupts the handle and
crashes the camera process. The correct access path is the handle's own accessor API:

```
DarwinnApi2_Buffer_SizeBytes(handle)              -> uint64               (real tensor size)
DarwinnApi2_Buffer_MapToHost(handle, &outPtr)     -> int32 status         (real host pointer)
DarwinnApi2_Buffer_FlushCache(handle)              -> int32 status         (commit writes to device)
```

`MapToHost` is **not safe to call synchronously from inside a Frida `Interceptor.attach`
`onLeave` callback** — that triggers a Frida re-entrancy fault (the same class of issue
documented for `RegisterGraph6` on the G2). Deferring the call with `setTimeout(fn, 0)` to
exit the interceptor stack first resolves it cleanly, with no crash and a confirmed write
to the real mapped pointer.

---

## Technical environment

| Property | Value |
|---|---|
| Device | Google Pixel 8 |
| SoC | Google Tensor G3 ("Rio" NPU) |
| Android version | 16 |
| Root | Magisk (init_boot patch) |
| Hook framework | Frida 17.9.5 |
| Target process | com.google.android.GoogleCamera |
| NPU library | libedgetpu_util.so (DarWINN API v2) |
| Input tensor | 224 × 224 × 3 uint8 (150,528 bytes) |
| Output tensors | 1–4 small scalar buffers (1–288 bytes) per inference pass |

## Scripts

| File | What it does |
|---|---|
| [`tpu_proof_g3.js`](tpu_proof_g3.js) | Hooks `AddOutput`/`Submit`, confirms the NPU is actively running inference per frame. |
| [`buffer_probe_g3.js`](buffer_probe_g3.js) | Read-only: resolves real tensor size and host pointer via the `Buffer` accessor API, deferred to avoid the re-entrancy fault. |
| [`inject_v3_g3.js`](inject_v3_g3.js) | The bidirectional proof: writes a known pattern into the live input tensor via `MapToHost` + `FlushCache`, deferred. |
