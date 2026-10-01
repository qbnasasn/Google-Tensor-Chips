# Cloud TPU (XLA) as a reference oracle for the G4 DGC0 weight format

The on-device DarWINN NPU and Google's Cloud TPU are both "TPU," both compiled by **XLA**. They are
**not** the same silicon — Cloud TPU is a big MXU + HBM part; DarWINN / EdgeTPU is the edge part with
embedded RISC-V "DiVE" vector cores (see [`DOORS.md`](DOORS.md), [`VII_AND_IOCTL.md`](VII_AND_IOCTL.md)).
But the XLA *compiler* layer — tiling, layout, quant packing — is family-shared. So a real Cloud TPU
is a legitimate **reference oracle** for how Google tiles and packs a quantized matmul, which we can
diff against the DGC0 weight format reverse-engineered in [`ternary-packer/`](ternary-packer/).

## Method

JAX, JIT a matmul on a real **Cloud TPU v6e (Trillium)**, dump every XLA stage with
`XLA_FLAGS=--xla_dump_to=...` (before/after optimizations, after codegen, `tpu_comp_env`). Probed
int8, bf16, native `jnp.int4`, int4-packed, and fp8.

## What XLA emits for int4 on Cloud TPU

The int4 weight parameter, straight from the HLO:

```
s4[K, N]{1,0: T(64,128)(8,1) E(4)}
```

- **`s4`** — two's-complement signed int4 (−8 … 7).
- **`T(64,128)`** — a 64×128 storage tile (contraction × **128 lanes**); `copy`-retiled to
  **`T(8,128)`** for the matmul.
- **`(8,1) E(4)`** — 8 int4 elements (`E(4)` = 4-bit) packed per 32-bit word in the minor dim
  (= 2 per byte), in **natural `W₀, W₁, …` order**.
- The matmul lowers to a **`convolution`** (`dim_labels=bf_io->bf`); systolic emitter
  `EmitAllBatchInSublanes`, 128-wide windows.
- **No permutation.** Across the full 177-file dump there is **no** `transpose` / `gather` / `iota` /
  `permute` / `shuffle`; the only weight ops are `copy` (retile) and `bitcast`.

The tile scales with precision (the 128 lane width is constant):

| dtype | contraction tile |
|-------|-----------------|
| bf16 | `T(8,128)(2,1)` |
| int8 | `32,128` |
| int4 | `64,128` |

Lower precision packs more rows per 128-lane row — the quant-packing trick, in Google's own compiler
output.

## What the DGC0 does for int4 (DarWINN)

From [`ternary-packer/`](ternary-packer/) (the ZOut layout, validated 64/64 against vendor compiles):

- **2 int4 / byte**, contraction `row = k // 2` — same density.
- **bias-8 encoding**: `nibble = (signed + 8) & 0xF` (0 … 15) — **not** two's-complement.
- **bit-swap(0,1) channel interleave**: output channels visit in order `[0,2,1,3,4,6,5,7]`
  (`channel = (p&4) | ((p&1)<<1) | ((p>>1)&1)`) — the DarWINN MMA channel tiling.

## The diff

| | Cloud TPU (XLA) | G4 DGC0 (DarWINN) | shared? |
|---|---|---|---|
| lane width | 128 | 128 | ✅ |
| int4 tile | 64×128 → 8×128 | 128-wide family | ✅ |
| packing density | 4-bit, 8/word (2/byte) | 4-bit, 2/byte | ✅ |
| matmul form | `convolution` | same lineage | ✅ |
| **value encoding** | two's-complement s4 | **bias-8 (+8)** | ❌ |
| **channel order** | identity (no permute) | **bit-swap(0,1)** | ❌ |

## Conclusion

The **tile geometry is shared XLA-family** — 128 lanes, a precision-scaled contraction tile, 4-bit
`E(4)` packing, matmul-as-convolution. The DGC0 then applies **two EdgeTPU-specific transforms that
XLA's Cloud TPU backend does not**: a **+8 bias** on each nibble, and a **bit-swap(0,1)** of the
channel index. Those are DarWINN's own — how its RISC-V DiVE vector units want the weights
pre-arranged for the MMA — and the vendor's on-device compiler bakes them into the DGC0 bytes.

So the exact bridge from an XLA int4 tile to a DGC0 int4 tile is:

> **`nibble = value + 8`, then permute the channel index by `bit-swap(0,1)`.**

Two cheap transforms on top of a shared scaffold. *(Scope: this is about the weight **format** /
geometry — not a claim about model output quality.)*
