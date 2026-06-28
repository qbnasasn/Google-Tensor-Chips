# Running an LLM On-Device — No Root Required

The NPU reverse-engineering in [`g2-pixel7/`](../g2-pixel7/) and [`g3-pixel8/`](../g3-pixel8/)
needs root. **This part doesn't.** It's the actual LLM serving setup that the NPU work was
trying (and failing) to accelerate — plain [llama.cpp](https://github.com/ggerganov/llama.cpp)
built and run inside Termux, no root, no Magisk, no Frida.

If you just want a local LLM running on a Pixel phone, this is the part to use. The NPU folders
are for people curious about the reverse-engineering, not a prerequisite.

## What you get

A local OpenAI-compatible chat API (`llama-server`) running on-device, CPU-only, fast enough to
be usable interactively.

| Device | SoC | Optimal threads | `llama-bench` decode speed | Real `llama-server` decode speed |
|---|---|---|---|---|
| Pixel 7 | Tensor G2 | `-t 4` | ~13–21 t/s (pinning-dependent) | ~9 t/s |
| Pixel 8 | Tensor G3 | `-t 5` | ~19.4–22 t/s | ~7 t/s |

Model used for these numbers: Qwen2.5-1.5B-Instruct, Q4_K_M quantization (~1 GB). Any
llama.cpp-compatible GGUF model of similar size will behave similarly; larger models scale down
roughly with parameter count.

**Note on the two speed columns:** `llama-bench` measures the raw forward-pass decode loop only.
`llama-server`'s real chat-completion throughput is consistently lower on both phones — confirmed
not to be caused by sampling settings, memory-fit heuristics, or CPU core placement (all tested,
none changed the result). The gap comes from `llama-server`'s own request/slot-management
overhead between tokens, not from anything device-specific. If you're benchmarking your own setup,
measure against the server you'll actually run, not `llama-bench` alone.

We also tested NPU offload for the compute-heavy part of decode (see the chip-specific folders)
— it does not help. Memory bandwidth, not compute, is the bottleneck for LLM decode on this
class of phone, and the NPU shares the same memory bus as the CPU. This setup intentionally
stays CPU-only because that's the part that's actually faster.

## Setup (Termux, no root)

1. Install [Termux](https://termux.dev/) from F-Droid (not the unmaintained Play Store build).
2. Bootstrap build tools:
   ```sh
   pkg update -y
   pkg install -y python clang cmake ninja git wget curl make
   ```
3. Clone and build llama.cpp natively for your phone's CPU:
   ```sh
   git clone https://github.com/ggerganov/llama.cpp ~/llama.cpp
   cd ~/llama.cpp
   cmake -B build -DCMAKE_BUILD_TYPE=Release -DGGML_NATIVE=ON \
       -DGGML_VULKAN=OFF -DGGML_OPENMP=OFF \
       -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
   cmake --build build --config Release -j$(nproc)
   ```
4. Download a GGUF model (e.g. Qwen2.5-1.5B-Instruct Q4_K_M) into `~/models/`.
5. Run the server, threads tuned per the table above:
   ```sh
   ~/llama.cpp/build/bin/llama-server \
       -m ~/models/qwen2.5-1.5b-instruct-q4_k_m.gguf \
       -c 2048 --host 127.0.0.1 --port 8090 \
       -n 512 -t 5 --temp 0.7 --repeat-penalty 1.1
   ```
6. Talk to it:
   ```sh
   curl http://127.0.0.1:8090/v1/chat/completions \
       -H "Content-Type: application/json" \
       -d '{"messages":[{"role":"user","content":"hello"}]}'
   ```

## Finding your own optimal thread count

`GGML_NATIVE=ON` auto-detects your CPU's instruction set (NEON/dotprod/i8mm) at build time, but
the right thread count is device-specific — going past the big/medium core count and spilling
onto efficiency cores usually makes things slower, not faster. Sweep `-t` from 2 up to your
core count and benchmark decode tokens/sec at each value; the optimum is usually one or two
threads short of the full core count.
