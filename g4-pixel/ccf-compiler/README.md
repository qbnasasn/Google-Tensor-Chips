# On-device DarWINN AOT compiler (driver)

These are the **drivers** for Google's on-device DarWINN tflite compiler. They `dlopen` the vendor
compiler blob and call its C ABI to turn a `.tflite` into a **DGC0** (DarWINN Graph Container — the
compiled artifact the hardware runs), entirely on the phone. No cloud SDK, no x86 toolchain.

The compiler blob itself is **not in this repo** — see [`BLOB_INSTRUCTIONS.md`](BLOB_INSTRUCTIONS.md)
to copy `libedgetpu_tflite_compiler.so` off your own device.

## Files

| File | What it does |
|------|--------------|
| `PixelG4A17_Compiler.c` | Minimal driver: dlopen blob → `CompileTfliteFlatbuffer2(tflite)` → DGC0 |
| `PixelG4A17_Compiler_flex.c` | Fuller driver — flexible I/O signatures, HW-version handling |
| `PixelG4A17_Compiler_hwver.c` | HW-version override (target a specific DarWINN rev) |
| `PixelG4A17_Compiler_nodie.c` / `_nodiepatch.c` | Crash-survival variants (compiler aborts on some graphs; these keep going / patch the abort) |
| `PixelG4A17_Compiler_probe.c` | Probes the compiler's C ABI / entry points |
| `msg_probe.c` | Probes the compiler's message / error-reporting interface |
| `build.sh` / `run.sh` | Build + invoke |

## Use

```sh
# 1. get the blob (see BLOB_INSTRUCTIONS.md), place it next to the driver
# 2. build
./build.sh
# 3. compile a tflite to a DGC0
./run.sh model.tflite        # -> model.dgc0
```

## Notes

- The on-device compiler is **single-subgraph**. For multi-subgraph graphs, split them and compile
  per-island, then let the dispatch stack load the DGC0s (see [`../aot-models/`](../aot-models/)).
- **Clear `/data/vendor/edgetpu/cache` before a fresh compile** — the cache is keyed on the model id,
  so a stale entry silently poisons a recompile.
- The DGC0 output runs on the NPU via the dispatch stack (Tachyon / VII — see
  [`../VII_AND_IOCTL.md`](../VII_AND_IOCTL.md)), with **no int8 ceiling** (that's an NNAPI-only limit)
  and no per-run recompile. This is how you get a sub-byte-quantized model onto the hardware.
