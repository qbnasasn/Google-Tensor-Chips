# Custom Model Execution on the DarWINN NPU — Capability & Performance Envelope

**Device**: Google Pixel 7 · Tensor G2 SoC · Janeiro NPU (DarWINN architecture)
**Demonstrated**: June 2026

---

## What this adds to the base package

The base SDK (`README_DEMO.md`) proves **bidirectional control of the NPU** while it runs
Google's own camera model. This addendum proves the next step:

> **Models authored by us — not Google — compile to and execute on the locked Tensor G2 NPU.**

This moves the capability from "puppet the camera's existing model" to "run arbitrary
custom networks on a closed proprietary accelerator."

---

## The proven path

A custom quantized model runs on the NPU through the device's own `google-edgetpu` NNAPI
accelerator. The full pipeline was validated end-to-end with a model we built from scratch:

1. **Author** an int8-quantized TFLite model (standard TensorFlow toolchain).
2. **Quantize correctly** for the Tensor G2 driver — the one non-obvious requirement:

   | Requirement | Value | Why |
   |---|---|---|
   | I/O type | `uint8` | driver rejects signed-int8 I/O |
   | Weight quant | **per-tensor** (not per-channel) | NNAPI `FULLY_CONNECTED` rejects per-axis weights → `ANEURALNETWORKS_BAD_DATA` |

   In the TF converter: `inference_input/output_type = tf.uint8` and
   `_experimental_disable_per_channel = True`.

3. **Execute** via NNAPI targeting accelerator `google-edgetpu`. The driver performs its own
   on-device compilation (stage-1 + stage-2 → DGC0) internally and runs the graph on the NPU.

**Confirmed on a real workload**: a transformer feed-forward block (3 matrix multiplies +
activation + element-wise ops, 41 MB of weights) compiled with **all six compute operations
delegated to the NPU hardware** — zero CPU fallback. On-device compilation time (~1.6 s init)
confirms genuine hardware compilation, not a software fallback.

---

## Performance envelope — measured, not estimated

The NPU is a **compute accelerator**. It wins decisively where the workload is compute-bound
and the model weights fit its on-chip memory (7.73 MB SRAM). It offers **no advantage** where
the workload is memory-bandwidth-bound, because it shares the LPDDR5 memory bus with the CPU.

| Workload class | Fits on-chip SRAM? | Bound by | NPU vs CPU |
|---|---|---|---|
| **Vision / CNN** (camera models, MobileNet-class) | Yes | compute | **NPU wins big** — its design point |
| **LLM decode** (large per-token weight streaming) | No (weights ≫ 8 MB) | memory bandwidth | **No win** — see below |

### LLM decode — the hard number

A single transformer FFN block, all compute ops on the NPU:

| Path | Latency / block | Effective bandwidth (41 MB weights) |
|---|---|---|
| **NPU** (google-edgetpu) | 1.81 ms | ~23 GB/s |
| **CPU** (4 threads) | 1.57 ms | ~26 GB/s |

Both run at roughly half the 51 GB/s LPDDR5 peak and contend for the same bus. Large-language-
model token generation streams the full weight set from DRAM every token, so it is bound by
memory bandwidth — a regime where the NPU's systolic array sits idle waiting on memory and
provides no speedup. **For on-device LLMs, the CPU is the correct target; the NPU is not.**

---

## What a buyer should take from this

- **The NPU is fully open to custom models**, not just observable. The access demonstrated in
  the base package extends to authoring and running your own networks on the hardware.
- **The capability is correctly scoped.** Where the silicon helps (real-time vision, sensor
  fusion, CNN inference) it is a strong, low-power accelerator running at the same privilege as
  Google's own pipeline. Where it does not (memory-bound LLM decode) is identified with a
  measurement, not a guess.
- **The performance boundaries are characterized**, so integration decisions can be made on
  data rather than trial and error.

---

## Technical environment

| Property | Value |
|---|---|
| Accelerator | `google-edgetpu` (NNAPI device 0) |
| Quantization | int8 weights, per-tensor; uint8 I/O |
| Validation tool | `benchmark_model` (TFLite), on-device |
| On-chip SRAM | 7.73 MB (weight cache ceiling) |
| Memory | LPDDR5, ~51 GB/s peak (shared SoC-wide) |
| Confirmed workloads | single FULLY_CONNECTED; full SwiGLU FFN block (6 ops, all delegated) |
