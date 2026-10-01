# Sub-byte weight packer (int4 / int2 / ternary)

`zout_emitter.py` emits weights in the DarWINN **MMA tile layout** for sub-byte quantization. That
layout isn't documented anywhere. It was recovered by compiling distinct-valued reference tensors
with the vendor compiler, reading how the bytes came back out, and reducing it to a closed form —
**validated 64/64** on 8×8 int4 blocks.

## The key result

For int4 (2 elements per byte), the channel visiting order across a tile is a **bit-swap(0,1)
permutation** of the column index:

```
channel = (p & 4) | ((p & 1) << 1) | ((p >> 1) & 1)
```

That permutation *is* the hardware's MMA channel tiling. Value encoding is int4 with bias 8:
`nibble = (signed_value + 8) & 0xF`.

`elements_per_byte = 8 / num_bits`, so the same structure packs **int2 at 4/byte** and **int1 at
8/byte**, with the analogous bit permutation.

## Why it matters

Without this, the compiler widens sub-byte weights toward int8 and you lose the footprint. With it,
you can **hand-author a packed DGC0 weight block** at int4 / int2 / ternary that the NPU reads
natively — which is how you get a true sub-byte model onto the hardware (NNAPI can't express
anything below int8; the AOT/DGC0 path can).

## Usage

See the module docstring and `int4_pos(oc, k)` — it returns `(physical_row, byte_in_row,
nibble_half)` for a logical weight `W[oc][k]`. Build a packed block by iterating logical weights and
placing each at its `int4_pos`.
