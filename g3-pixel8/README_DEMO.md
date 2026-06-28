# DarWINN NPU Access — Tensor G3 (Pixel 8)

**Device**: Google Pixel 8 · Tensor G3 SoC · DarWINN NPU
**Demonstrated**: June 2026

---

## What was proven

Read access to the Tensor G3 NPU while it ran Google Camera's live portrait-mode pipeline,
plus a write call that succeeds cleanly at the API level — reached via a different internal
API than [`../g2-pixel7/README_DEMO.md`](../g2-pixel7/README_DEMO.md)'s G2 proof. The result is
weaker than G2's on the write side; see the honest caveat below.

1. **Read — confirmed working**: intercepted the live inference loop and read real tensor
   bytes from both input (150,528 bytes — a 224×224×3 camera frame) and output buffers (small
   scalar results, 1–288 bytes, varying by inference pass). Verified across hundreds of frames.
2. **Write — API call succeeds, causal effect not confirmed**: wrote known patterns into the
   live input tensor (a 16-byte corner write, then a full-tensor 150,528-byte fill, then the
   same full-tensor fill repeated on every frame for 300+ frames) and flushed via
   `FlushCache`, which returns success every time with zero crashes or instability. Unlike G2,
   where the equivalent write produced an immediate, visible output flip, **none of these G3
   writes produced any detectable change in the output tensors** — output bytes stayed
   statistically identical to the uninjected baseline throughout. Most likely explanation:
   `MapToHost` on G3 returns a host-side staging copy decoupled from the buffer the hardware
   actually reads for inference, and `FlushCache` flushes CPU cache lines rather than
   performing a host→device sync. Not root-caused further — see `buffer_probe_g3.js` and
   `inject_v4_g3.js` for the full test sequence.
3. **No instability either way**: across all read and write tests, Google Camera was never
   crashed or corrupted by this tooling.

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
| [`inject_v3_g3.js`](inject_v3_g3.js) | Write attempt 1: a 16-byte pattern into the corner of the live input tensor via `MapToHost` + `FlushCache`, deferred. No crash; no detectable output change. |
| [`inject_v4_g3.js`](inject_v4_g3.js) | Write attempts 2 and 3: same mechanism, escalated to a full 150,528-byte `0x80` fill, first once then on every frame for 300+ frames. No crash either way; still no detectable output change — see the caveat above. |
