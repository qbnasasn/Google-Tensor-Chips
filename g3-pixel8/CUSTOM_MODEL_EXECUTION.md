# LLM Decode on the Tensor G3 NPU — Bandwidth Verdict

**Device**: Google Pixel 8 · Tensor G3 SoC
**Demonstrated**: June 2026

---

## Same question as G2, re-tested on newer silicon

[`../g2-pixel7/CUSTOM_MODEL_EXECUTION.md`](../g2-pixel7/CUSTOM_MODEL_EXECUTION.md) measured
whether offloading a transformer FFN block (the bulk of LLM decode compute) to the NPU beats
running it on the CPU, on the Tensor G2. The same int8/per-tensor-quantized FFN block was
re-run on the Tensor G3 via the `google-edgetpu` NNAPI accelerator to see if newer silicon
changes the answer.

It doesn't.

| Path | Latency / block (G2) | Latency / block (G3) |
|---|---|---|
| **NPU** (google-edgetpu) | 1.81 ms | 2.01 ms |
| **CPU** | 1.57 ms | 1.66 ms |

The NPU is still ~15–20% slower than the CPU for this workload on both chips. The reason is
unchanged: LLM decode streams the full weight set from DRAM every token — it's bound by memory
bandwidth, not compute, and the NPU shares the same memory bus as the CPU. A faster NPU doesn't
help a bandwidth-bound workload; it just waits on the same bus a little faster.

**Verdict holds across generations: keep LLM decode on the CPU. The NPU remains the right tool
for compute-bound, on-chip-SRAM-sized workloads (vision/CNN), not for streaming a multi-gigabyte
weight set token by token.**
