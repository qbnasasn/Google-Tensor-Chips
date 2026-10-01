# Getting the compiler blob

The driver here (`PixelG4A17_Compiler*.c`) drives Google's on-device DarWINN tflite compiler. That
compiler itself — **`libedgetpu_tflite_compiler.so`** — is Google proprietary and is **NOT included
in this repo.** You already have it on any Tensor-G4 device; copy it off your own hardware:

```sh
# from a host with a rooted device:
adb root
adb pull /vendor/lib64/libedgetpu_tflite_compiler.so ./

# or, on-device from a root shell (Termux + su):
su -c 'cp /vendor/lib64/libedgetpu_tflite_compiler.so .'
```

It's ~60 MB. Place it next to the driver, then build with `build.sh`.

## What the driver does

It `dlopen()`s the blob and calls its public C ABI (`CompileTfliteFlatbuffer2`) to compile a
`.tflite` into a **DGC0** — a DarWINN Graph Container, the compiled artifact the hardware actually
runs. No cloud SDK, no x86 toolchain; the whole compile happens on the phone.

See `PixelG4A17_COMPILER` notes and `run.sh` for invocation. The compiler is single-subgraph on
device; multi-subgraph graphs are split and compiled per-island (see `../aot-models/`).

**Do not redistribute the blob.** It is Google's, not ours — this repo ships only the *driver* that
calls it. Device-owner use on your own hardware only.
