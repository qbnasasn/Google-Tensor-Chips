# Google Tensor Chips — DarWINN NPU Access

Direct-access tooling for the **Google Tensor G2 NPU** (DarWINN / Janeiro) on the Pixel 7.

## Why this exists

I was trying to get **LLM inference running on the phone** and wanted to use the Tensor G2 NPU
to speed it up. To do that I had to reverse-engineer how the NPU is actually driven, since it has
no public API — only Google Camera and signed system services are allowed to use it.

Along the way I learned the NPU **doesn't help with LLMs**: language-model decoding is
memory-bandwidth-bound, and the NPU shares the same memory bus as the CPU, so it ends up no faster
(measured ~1.81 ms/block on the NPU vs ~1.57 ms on the CPU for a real transformer FFN block). For
my own use case that was a dead end — the LLM runs better on the CPU.

But the access tooling itself works, and the NPU **is** a strong accelerator for the workloads it
was built for (vision / CNN models, where the weights fit on-chip). It didn't serve my project, but
others doing on-device vision or NPU research might find it useful — so I'm releasing it under MIT.

## What's here

| File | What it is |
|---|---|
| [`README_DEMO.md`](README_DEMO.md) | Walkthrough of the bidirectional read/write access to the NPU while it runs Google Camera's live inference. |
| [`CUSTOM_MODEL_EXECUTION.md`](CUSTOM_MODEL_EXECUTION.md) | How to run your **own** quantized models on the NPU, plus the measured performance envelope. |
| [`resolve_offsets.js`](resolve_offsets.js) | Frida 17.x runtime symbol resolver (works across APK versions). |
| [`darwinn_sdk.js`](darwinn_sdk.js) / [`darwinn_sdk.md`](darwinn_sdk.md) | Small SDK wrapping NPU read / write / inject / info. |
| [`darwinn_runner.cpp`](darwinn_runner.cpp) / [`darwinn_runner.md`](darwinn_runner.md) | Standalone C++ runner — load a DGC0 model, serve inference over a socket (no Frida, no GCamera). |
| [`janeiro_open_test.cpp`](janeiro_open_test.cpp) | Device-node open/probe test for `/dev/janeiro`. |

## What I found, in short

- The NPU can be driven directly from a rooted device, at the same privilege as the Camera app.
- **Custom models run on it**: int8 weights, uint8 I/O, **per-tensor** quantization → loadable via
  the `google-edgetpu` NNAPI accelerator (per-channel weights get rejected).
- **It's a compute accelerator, not a memory one.** Great for vision/CNN (weights fit the 7.73 MB
  on-chip SRAM). No help for LLM decode (bandwidth-bound, shares the LPDDR5 bus with the CPU).

## Environment

Google Pixel 7 · Tensor G2 (DarWINN/Janeiro) · Android 14 · Magisk root · Frida 17.x ·
target process `com.google.android.GoogleCamera` · `/vendor/lib64/libedgetpu_util.so`.

Requires root — this is device-owner access to your own hardware, for research and learning.

## License

[MIT](LICENSE). Shared as-is, no warranty, in case it helps someone.
