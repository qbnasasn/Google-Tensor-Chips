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

## Target build — read before running

The `PixelG4A17_Compiler*.c` drivers patch the vendor compiler **in memory** (nothing is written
to `/vendor` on disk) at fixed offsets. Those offsets fit exactly one build of
`libedgetpu_tflite_compiler.so` — per the source comments, the Android 17 build on a Pixel 9
(Tensor G4), reverse-engineered 2026-07-01.

| | |
|---|---|
| Device | Pixel 9 (Tensor G4) |
| Android build ID | *not yet provided by the contributor* |
| Compiler `.so` size | *not yet provided* |
| Compiler `.so` SHA-256 | *not yet provided* |

Safeguards:

- Every patch checks the original instruction bytes before writing. On any mismatch the driver
  now **aborts before calling the compiler** (exit 2) instead of running half-patched. Set
  `ALLOW_PATCH_MISMATCH=1` only if you are doing your own reverse-engineering.
- `run.sh` prints the size and SHA-256 of your `.so`. Set `COMPILER_SHA256=<hex>` to make it
  refuse any other build outright.
- `msg_probe.c` writes a `brk #0` at a fixed offset (`0x39c8014`) **without** an original-byte
  check. Only use it against the same build, and verify the offset first.

If you run this on a different build, please report your device, build ID and `.so` hash in an
issue.

## SELinux

`run.sh` does **not** touch SELinux by default. On some builds the compiler is denied access to
`/vendor` or `/data/vendor/edgetpu` when launched from a root shell; if that happens you can opt
in with `PERMISSIVE=1 ./run.sh ...`, which sets SELinux to permissive **device-wide** for the
duration of the run and restores enforcing afterwards. Don't use it on a device you rely on
without understanding that trade-off.

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
