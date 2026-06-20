# Google Tensor Chips — DarWINN NPU Access

Reverse-engineering and direct-access tooling for the **Google Tensor G2 NPU** (DarWINN /
Janeiro architecture) on the Pixel 7. Researched on a rooted device; released here under the
MIT license.

## What's here

| File | What it is |
|---|---|
| [`README_DEMO.md`](README_DEMO.md) | Proof of capability — full bidirectional read/write access to the Tensor G2 NPU while it runs Google Camera's live inference pipeline. |
| [`CUSTOM_MODEL_EXECUTION.md`](CUSTOM_MODEL_EXECUTION.md) | Running **your own** quantized models on the NPU via the `google-edgetpu` NNAPI accelerator, plus the measured performance envelope (strong for vision/CNN, bandwidth-bound for LLM decode). |
| [`resolve_offsets.js`](resolve_offsets.js) | Frida 17.x runtime symbol resolver — APK-version-independent DarWINN offset resolution. |
| [`darwinn_sdk.js`](darwinn_sdk.js) / [`darwinn_sdk.md`](darwinn_sdk.md) | RPC-callable SDK wrapping the NPU access into clean read/write/inject/info calls. |
| [`darwinn_runner.cpp`](darwinn_runner.cpp) / [`darwinn_runner.md`](darwinn_runner.md) | Standalone C++ runner — opens a DarWINN VirtualDevice, loads a DGC0 model, serves inference over a Unix/TCP socket (no Frida, no GCamera). |
| [`janeiro_open_test.cpp`](janeiro_open_test.cpp) | Device-node open/probe test for `/dev/janeiro`. |

## Key findings

- The Tensor G2 NPU has **no public API** — only Google Camera and signed system services
  may use it. This tooling reaches the hardware at the same privilege as the Camera app, by
  attaching to its process and hooking the internal DarWINN API.
- **Custom models run on it**: int8, uint8 I/O, **per-tensor** weight quantization → loadable
  via the `google-edgetpu` NNAPI accelerator (per-channel weights are rejected).
- **Performance envelope is measured, not guessed**: the NPU is a compute accelerator that wins
  on compute-bound vision/CNN workloads (weights fit the 7.73 MB on-chip SRAM), but offers no
  benefit on memory-bandwidth-bound LLM decode — it shares the LPDDR5 bus with the CPU.

## Environment

Google Pixel 7 · Tensor G2 (DarWINN/Janeiro) · Android 14 · Magisk root · Frida 17.x ·
target process `com.google.android.GoogleCamera` · `/vendor/lib64/libedgetpu_util.so`.

## License

[MIT](LICENSE). Research/educational release. Provided as-is, no warranty.
