"""ZOut sub-byte weight PACKER — solved from beta compiles of distinct-valued refs (2026-07-25).
The exact int4 (N=8,K=8) permutation was read from ref_oc_packed/ref_k_packed and reduced to a
closed form, validated 64/64. This is the emitter core for hand-authoring packed DGC0 weight blocks.

KEY STRUCTURE (int4, elements_per_byte=2):
  physical_row = k // 2 ;  kpar = k % 2
  within a row: 8 bytes = 4 byte-pairs x 2 (byte = byte_pair*2 + kpar)
  byte_pair bp (0..3) -> base channel [0,1,4,5][bp]; the byte's two nibbles = {base(lo), base+2(hi)}
  => channel visiting order across the 8 nibble-columns (fixed k) = [0,2,1,3,4,6,5,7]
     = bit-swap(0,1) of the column index p:  channel = (p&4) | ((p&1)<<1) | ((p>>1)&1)
  value encoding: nibble = (signed_value + 8) & 0xF   (int4 bias 8)

This bit-swap interleave is the hardware MMA channel tiling. For int1 (epb=8) one byte holds all 8
channels across 8 bits; the bit-order is the analogous channel bit-permutation (candidates tested on the
NPU oracle). elements_per_byte = 8/num_bits (decompiled invariant, opencode).
"""

def int4_pos(oc, k, N=8, K=8):
    """Return (physical_row, byte_in_row, nibble_half) for logical W[oc][k], int4 N=8 K=8. VALIDATED 64/64."""
    row  = k // 2
    kpar = k % 2
    nib  = 1 if (oc % 4) >= 2 else 0     # hi-nibble if oc%4 in {2,3}
    base = oc - 2 * nib                  # 0,1,4,5
    bp   = (base // 4) * 2 + (base % 2)  # byte-pair 0..3
    byte = bp * 2 + kpar
    return row, byte, nib

def pack_int4(W, N=8, K=8, bytes_per_row=16):
    """Pack W[N][K] into the ZOut int4 layout. Returns list of (bytes_per_row)-byte rows."""
    nrows = (K // 2)
    buf = [bytearray(bytes_per_row) for _ in range(nrows)]
    for oc in range(N):
        for k in range(K):
            row, byte, nib = int4_pos(oc, k, N, K)
            code = (W[oc][k] + 8) & 0xF
            buf[row][byte] |= code << (4 * nib)
    return buf

if __name__ == "__main__":
    # Round-trip validation against the actual beta-compiled reference blocks.
    oc = open("ref_oc_packed.tflite", "rb").read()
    kk = open("ref_k_packed.tflite", "rb").read()
    rows = [0x4f80, 0x4f90, 0x4fa0, 0x4fb0]
    # Reconstruct W for each ref from the known generator (Woc[oc][k]=oc-4, Wk[oc][k]=k-4)
    N = K = 8
    Woc = [[oc_ - 4 for _ in range(K)] for oc_ in range(N)]
    Wk  = [[k_ - 4 for k_ in range(K)] for _ in range(N)]
    for name, W, blob in (("ref_oc", Woc, oc), ("ref_k", Wk, kk)):
        packed = pack_int4(W, N, K, bytes_per_row=16)
        ok = True
        for ri, base in enumerate(rows):
            actual = blob[base:base + 16]
            if bytes(packed[ri]) != bytes(actual):
                ok = False
                print(f"{name} row{ri} MISMATCH\n  emit  {packed[ri].hex()}\n  actual{actual.hex()}")
        print(f"{name}: round-trip {'PASS' if ok else 'FAIL'}")
