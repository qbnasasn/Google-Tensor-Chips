# Every door into the Tensor G4 NPU

There is no public API for the DarWINN NPU — only Google Camera and signed system services are
meant to use it. But there are **four distinct ways in**, at different layers, with different
trade-offs.

## 1. NNAPI — `google-edgetpu` (sanctioned, no root)

The NPU enumerates as an Android NNAPI device: `google-edgetpu` (feature level 1000008, v2.0),
visible from plain userspace. Hand it a graph and the vendor **DarWINN driver compiles it internally
at `prepareModel()`**. This is Google's own production door.

- **Pro**: no root, no cracked anything, officially supported.
- **Con**: caps at **int8 + fp16** (the operand-type enum has nothing below int8); subject to the
  ~60s on-device compile deadline (see [`NNAPI_60S_WALL.md`](NNAPI_60S_WALL.md)).
- Drive it from C with no JVM: [`scripts/nnapi_maxpart.c`](scripts/nnapi_maxpart.c).

## 2. The Tachyon HAL

`com.google.edgetpu.tachyon-service` is the compute service your runtime talks to over binder. You
pass it weight/activation dmabufs; it does the actual device ioctls in **its** process. The
client-side API is `CreateSession` + FMQ command/response queues. This is the layer LiteRT's
dispatch path uses.

## 3. Raw VII ioctl (`/dev/edgetpu`)

One layer below the HAL: the raw DarWINN **work-submit ioctl**, a.k.a. "VII" — `0xc050ed23` =
`_IOWR(0xED, 0x23, 80B)`. Fully decoded, with the whole `0xED` command set, in
[`VII_AND_IOCTL.md`](VII_AND_IOCTL.md). This path is **quant-agnostic** — it submits a compiled
graph (DGC0) regardless of bit-width, so it has **no int8 ceiling**. Submitting VII yourself means
registering your own DarWINN client, which the HAL normally does for you.

## 4. Frida into Google Camera

Piggyback on a process that is *already allowed* to use the NPU — `com.google.android.GoogleCamera`
— and call the DarWINN API from inside it. This is the approach the [`../g2-pixel7/`](../g2-pixel7/)
and [`../g3-pixel8/`](../g3-pixel8/) folders use. Needs root + Frida; no custom compiler required.

---

## Which door for what

| Goal | Door |
|------|------|
| Run an int8 / fp16 graph, officially, no root | NNAPI (1) |
| Run a **sub-byte** (int4/int2/ternary) graph | AOT-compile to DGC0 ([`ccf-compiler/`](ccf-compiler/)) → dispatch via Tachyon/VII (2/3) |
| Read/inject a live tensor in a running model | Frida into Camera (4) |
| Understand/trace what's hitting silicon | VII ioctl trace (3) |
