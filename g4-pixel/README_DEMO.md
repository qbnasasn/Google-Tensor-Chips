# DarWINN NPU — Tensor G4 (Pixel)

**Device**: Google Pixel · Tensor **G4** SoC · DarWINN NPU · Android, Magisk root

Where [`../g2-pixel7/`](../g2-pixel7/) and [`../g3-pixel8/`](../g3-pixel8/) reverse-engineered
*read/write access* to the NPU while Google Camera drove it, this folder goes further: **driving the
NPU directly, compiling custom graphs to it ahead-of-time, and dispatching sub-byte-quantized graphs
on it** — the parts the top-level README calls a dead end for anything but vision.

> ## ⚠️ Read this first — scope & what is NOT claimed
> This folder is about getting graphs to **compile and dispatch on the NPU**: the compiler path, the
> VII ioctl/dispatch mechanism, sub-byte weight packing, and the compile-cache that beats the ~60s
> wall. **It is NOT a claim of coherent model output.**
>
> **Coherent generation at aggressive sub-byte quant (int4/int2/ternary) is UNSOLVED.** Several of
> the artifacts here compile cleanly and run on the NPU but produce **incoherent / garbage text.**
> "Compiles + dispatches on the NPU" and "generates coherent text" are two completely separate
> claims — **only the first is made here.** Don't read any benchmark, DGC0, or compile result in this
> repo as a working, coherent on-device LLM. It isn't one yet.

## What's here

| Path | What it is |
|------|-----------|
| [`VII_AND_IOCTL.md`](VII_AND_IOCTL.md) | The raw DarWINN work-submit ioctl ("VII"), the `0xED` command set decoded — and where it actually fires |
| [`DOORS.md`](DOORS.md) | Every way into the G4 NPU: NNAPI `google-edgetpu`, the Tachyon HAL, raw VII ioctl, Frida-into-Camera |
| [`NNAPI_60S_WALL.md`](NNAPI_60S_WALL.md) | Beating the ~60s on-device compile deadline by warming the compile cache (99s → 4.8s) |
| [`ccf-compiler/`](ccf-compiler/) | On-device AOT compiler driver: turns a tflite into a DarWINN graph container (DGC0). Vendor compiler blob **not included** — see [`ccf-compiler/BLOB_INSTRUCTIONS.md`](ccf-compiler/BLOB_INSTRUCTIONS.md) |
| [`ternary-packer/`](ternary-packer/) | The sub-byte weight-tile packer (int4 / int2 / ternary), closed-form, validated 64/64 |
| [`aot-models/`](aot-models/) | Example AOT-compiled DGC0 graph containers produced by the above |
| [`scripts/`](scripts/) | `nnapi_maxpart.c` — drive the NNAPI delegate with full options (accelerator, partitions, cache) from C, no JVM |
| [`CLOUD_TPU_REFERENCE.md`](CLOUD_TPU_REFERENCE.md) | XLA on a real Cloud TPU (v6e) as a reference oracle — how it tiles+packs int4, and the exact `+8`/bit-swap bridge to the DGC0 weight format |

## The short version

- The G4 NPU is reachable **four different ways** (`DOORS.md`). NNAPI `google-edgetpu` is the
  sanctioned one, but it caps at **int8 / fp16**. The raw VII ioctl path and the AOT/DGC0 path do
  not.
- Custom graphs **compile to the NPU ahead-of-time** (`ccf-compiler/`) into DGC0 containers that the
  dispatch stack loads and runs — no per-run compile, and no int8 ceiling.
- Sub-byte weights (int4 / int2 / ternary) pack into the hardware MMA tile layout with a
  **closed-form permutation** (`ternary-packer/`) recovered from the compiler's own output and
  validated exhaustively on 8×8 blocks.
- The on-device JIT compile deadline (~60s) is beaten by **caching the compiled result and warming
  it** (`NNAPI_60S_WALL.md`): first compile 99s, every reload 4.8s.

Everything here is **device-owner research on your own hardware.** No Google proprietary binaries are
redistributed — where the vendor compiler blob is needed, instructions tell you how to copy it off
your own device.
