# Beating the ~60s on-device compile deadline

When you hand a graph to the `google-edgetpu` NNAPI device, the vendor DarWINN driver **compiles it
on the phone** at `prepareModel()`. For anything the size of an LLM that compile is slow, and there
is an effective **~60-second deadline** — a cold compile that overruns it loses the race.

This is real: a cold compile of a ~1B-parameter int8 transformer tflite measured **99.19s** — over
the line.

## The fix: warm the compile cache

The NNAPI delegate can persist its compiled result to a cache directory and reload it with
`prepareModelFromCache`. So you pay the long compile **once**, and every reload after is a cache hit:

| | compile time | result |
|---|---|---|
| cold | **99.19s** | compiles clean, loses the 60s race |
| warm (same cache dir) | **4.82s** | `prepareModelFromCache: successfully prepared model from cache` |

**99s → 4.82s** — roughly 20× under the deadline. The whole-model compile only has to survive the
60s window a single time; after that it's cached DGCs on disk.

## How

Drive the NNAPI delegate with the full option set — accelerator pinned to `google-edgetpu`, a
`cache_dir`, a cache token, `max_number_delegated_partitions`, `disallow_nnapi_cpu` — from C, with no
JVM, via [`scripts/nnapi_maxpart.c`](scripts/nnapi_maxpart.c). It builds a fake `JNIEnv` so it can
call the delegate's `createDelegate` JNI entry point directly.

```sh
# cold: compiles + populates the cache (slow, once)
nnapi_maxpart model.tflite 16 /path/to/cache 1 2
# warm: same args, same cache dir -> cache hit, sub-60s
nnapi_maxpart model.tflite 16 /path/to/cache 1 2
```

Args: `<model> <max_partitions> <cache_dir> <allow_fp16> <preference>`.

## Notes

- `max_partitions` controls how much of the graph is delegated vs. left on CPU — higher delegates
  more but fragments into more partitions (each a separate compile). There's a sweet spot per model.
- This is the **NNAPI (int8/fp16) door**. It does not lift the int8 ceiling — for sub-byte you AOT-
  compile to a DGC0 instead (see [`ccf-compiler/`](ccf-compiler/) and [`DOORS.md`](DOORS.md)). But
  for int8 models it's the cleanest way to a fast-loading, NPU-resident graph.
