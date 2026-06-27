# Google Tensor Chips — DarWINN NPU Access

Direct-access tooling for the **Google Tensor NPU** (DarWINN), reverse-engineered across two
generations: **Tensor G2** (Pixel 7, codename Janeiro) and **Tensor G3** (Pixel 8, codename Rio).

## Why this exists

I was trying to get **LLM inference running on the phone** and wanted to use the Tensor NPU to
speed it up. To do that I had to reverse-engineer how the NPU is actually driven, since it has no
public API — only Google Camera and signed system services are allowed to use it.

Along the way I learned the NPU **doesn't help with LLMs**, on either generation:
language-model decoding is memory-bandwidth-bound, and the NPU shares the same memory bus as the
CPU, so it ends up no faster (~1.81 ms/block NPU vs ~1.57 ms CPU on G2; ~2.01 ms vs ~1.66 ms on
G3, for a real transformer FFN block). For my own use case that was a dead end on both chips — the
LLM runs better on the CPU.

But the access tooling itself works, and the NPU **is** a strong accelerator for the workloads it
was built for (vision / CNN models, where the weights fit on-chip). It didn't serve my project, but
others doing on-device vision or NPU research might find it useful — so I'm releasing it under MIT.

## What's here

| Folder | What's there | Root required? |
|---|---|---|
| [`on-device-llm/`](on-device-llm/) | The actual LLM serving setup (llama.cpp + Qwen2.5-1.5B) that the NPU work below was trying to accelerate. Measured tokens/sec on both phones. | **No** |
| [`g2-pixel7/`](g2-pixel7/) | Tensor G2 (Pixel 7, Janeiro) NPU access — full SDK, standalone C++ runner, custom model execution, bidirectional proof. The original reverse-engineering work. | Yes |
| [`g3-pixel8/`](g3-pixel8/) | Tensor G3 (Pixel 8, Rio) NPU access — bidirectional proof ported to G3's different (handle-based) Buffer API, plus the re-measured FFN bandwidth verdict. | Yes |

If you just want a local LLM running on your phone, start with `on-device-llm/` — it's the
useful, no-root part. The `g2-pixel7/`/`g3-pixel8/` folders are for people curious about *why*
the NPU isn't used and how the locked hardware was accessed anyway.

The two chips' NPUs expose the **same DarWINN API v2 surface** (`AddInput`/`AddOutput`/`Submit`)
but differ in one important way: G2 hands back a raw tensor pointer, while G3 wraps tensors in an
opaque `Buffer*` handle that must be accessed through its own `SizeBytes`/`MapToHost`/`FlushCache`
API. See [`g3-pixel8/README_DEMO.md`](g3-pixel8/README_DEMO.md) for what that changes in practice.

## What I found, in short

- The NPU can be driven directly from a rooted device, at the same privilege as the Camera app —
  on both G2 and G3.
- **Custom models run on it** (demonstrated on G2): int8 weights, uint8 I/O, **per-tensor**
  quantization → loadable via the `google-edgetpu` NNAPI accelerator (per-channel weights get
  rejected).
- **It's a compute accelerator, not a memory one.** Great for vision/CNN (weights fit the on-chip
  SRAM). No help for LLM decode (bandwidth-bound, shares the LPDDR5 bus with the CPU) — confirmed
  on both generations.

## Environment

`on-device-llm/` needs nothing but Termux — no root. The NPU folders need a Magisk-rooted
Pixel 7 (Tensor G2) or Pixel 8 (Tensor G3) · Android 14/16 · Frida 17.x · target process
`com.google.android.GoogleCamera` · `libedgetpu_util.so` — device-owner access to your own
hardware, for research and learning.

## License

[MIT](LICENSE). Shared as-is, no warranty, in case it helps someone.
