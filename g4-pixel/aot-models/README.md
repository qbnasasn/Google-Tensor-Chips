# AOT-compiled DGC0 graph containers

**DGC0** = DarWINN Graph Container — the ahead-of-time compiled artifact the NPU actually runs,
produced by the on-device compiler in [`../ccf-compiler/`](../ccf-compiler/) and packed sub-byte with
[`../ternary-packer/`](../ternary-packer/). The dispatch stack (Tachyon / `VII_COMMAND`, see
[`../VII_AND_IOCTL.md`](../VII_AND_IOCTL.md)) just loads and submits it — no per-run compile.

## Sub-byte on the NPU — the point

The top-level README treats the NPU as useful for vision/CNN only. These DGC0s show the **compile +
dispatch** path for something else entirely: transformer graphs quantized to **int4 / int2 /
ternary**, AOT-compiled to DGC0 and dispatched via `VII_COMMAND`. NNAPI can't even express a weight
below int8 (it's an API-level operand-type limit); this path compiles sub-byte **straight to
silicon**, no int8 ceiling.

> **Honest scope:** this folder and the collection below are about the **compile + dispatch
> capability** — getting sub-byte graphs onto the NPU and running. Output *quality* at aggressive
> sub-byte quant (e.g. a 4B model at 2 bits/param) is a separate, ongoing problem; some of these
> artifacts compile and dispatch cleanly but don't yet generate coherent text. We keep the "it
> compiles and runs on the NPU" claim strictly separate from the "it's a good model" claim.

## The collection — toy models of every op × quant

The useful, systematic version of this is a **micro-benchmark suite**: one isolated single-op DGC0
per `(op, dtype, shape)` — Conv / DepthwiseConv / FullyConnected / MatMul / Softmax / Add / … across
int8 / int4 / int2 / fp16, per-tensor vs per-channel, shapes swept by stride — each AOT-compiled and
profiled on-device. That turns the compiler + hardware from a black box into an **empirical
micro-architectural map** (op-coverage, fusion patterns, tiling/alignment rules, a real roofline).

The compiled G4 DGC0 artifacts live here:

> **https://huggingface.co/xThr45hx/G4_DGCO**

## Reproduce

1. Quantize + pack sub-byte with [`../ternary-packer/`](../ternary-packer/).
2. AOT-compile each subgraph to a DGC0 with [`../ccf-compiler/`](../ccf-compiler/).
3. Bundle + dispatch via the DarWINN stack (`VII_COMMAND`).
