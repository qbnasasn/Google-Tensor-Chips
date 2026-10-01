# VII & the EdgeTPU ioctl interface — Tensor G4 (DarWINN)

This is the part everyone finds confusing, so it gets its own doc. **"VII" is the DarWINN
**`EDGETPU_VII_COMMAND`** ioctl — the raw "run this graph" submission to the NPU.** The confusion
comes from (a) not seeing which layer you're looking at, and (b) the ioctl numbers not matching any
obvious names. Both are cleared up below. The full, authoritative table (straight from the open-
source rio kernel UAPI, `drivers/edgetpu/edgetpu.h`) is in [`IOCTL_PROTOCOL.md`](IOCTL_PROTOCOL.md).

## The layers (top → silicon)

```
  your app / runtime                 ← hands weight+activation dmabufs over binder
      │  (NNAPI, or LiteRT dispatch)    (int8/fp16 ceiling lives HERE, at the NNAPI API)
      ▼
  DarWINN HAL service                ← com.google.edgetpu.tachyon-service
      │                                android.hardware.neuralnetworks@service-darwinn-aidl
      ▼
  /dev/edgetpu ioctl  ← **VII**      ← the HAL does the ioctls, NOT your app
      ▼
  EdgeTPU / DarWINN silicon
```

The single most important thing: **your process does not issue the NPU ioctls.** It passes buffers
to the HAL over binder, and the **HAL** (`tachyon-service`) issues the ioctls. So `strace` on your
own runtime shows *nothing* — you have to trace the HAL.

## Device nodes

```
/dev/edgetpu         -> /dev/edgetpu-soc  (symlink)
/dev/edgetpu-soc     char 488:1 (system)  ← the HAL-facing node, where VII fires
/dev/edgetpu-limited char 488:2 (system)  ← camera / limited clients
/dev/rio             char 488:0 (root)    ← G4 ("rio") privileged node
```

## The ioctl set (magic `0xED`) — the ones you actually see

Magic `0xED` = `EDGETPU_IOCTL_BASE`. Real names from the rio kernel UAPI (full table in
[`IOCTL_PROTOCOL.md`](IOCTL_PROTOCOL.md)). These are the ones that dominate a live trace:

| full `_IOC` value | name | nr | size | role |
|-------------------|------|----|------|------|
| `0xc028ed11` | `EDGETPU_MAP_DMABUF`   | 17 | 40 B | map an I/O tensor dma-buf → TPU virtual addr |
| `0x4028ed12` | `EDGETPU_UNMAP_DMABUF` | 18 | 40 B | mirror of the above |
| **`0xc050ed23`** | **`EDGETPU_VII_COMMAND`**  | **35** | **80 B** | **THE submit ioctl — "run this graph"** |
| `0xc018ed24` | `EDGETPU_VII_RESPONSE` | 36 | 24 B | drain the result (polled, so it's the most frequent) |
| `0x0000ed1a` / `0x0000ed19` | `ACQUIRE` / `RELEASE_WAKE_LOCK` | 26 / 25 | 0 | power |

> Earlier, from a raw trace alone, it's easy to misread these — e.g. calling `VII_RESPONSE` a
> "poll" or the dmabuf maps a "queue". The kernel header settles it. There are also
> `VII_LITEBUF_COMMAND`/`_RESPONSE` (nr 37/38) — a lighter 40 B submit variant.

## VII decoded

```
VII  =  EDGETPU_VII_COMMAND  =  0xc050ed23  =  _IOWR(0xED, 35, struct edgetpu_vii_command_ioctl)

  0xc050ed23
    dir  = 0b11  (READ|WRITE)
    size = 0x050 (80 bytes)
    type = 0xED  (magic)
    nr   = 0x23  (35)
```

The 80-byte struct is a **descriptor, not the data** (from `edgetpu.h`):

```c
struct edgetpu_vii_command {        // 48 B
    __u64 seq;                      //   matches the response.seq
    __u16 code;                     //   command type (FW/runtime-defined)
    __u8  priority;                 //   0..99, lower = higher priority
    __u8  reserved_0[5];
    struct { __u64 address; __u32 size; __u32 flags; } dma_descriptor;  // TPU VA from MAP_*
    __u8  reserved_1[8];
    __u32 client_id;                //   OVERRIDDEN by kernel; ignore
    __u8  qos_class, cluster_ids_bitset, atomic, reserved_2[1];
} __attribute__((packed));
struct edgetpu_vii_command_ioctl {  // 80 B
    struct edgetpu_vii_command command;
    __u64 in_fence_array;  __u32 in_fence_count;    // userspace int[] ptrs, max 64 each
    __u64 out_fence_array; __u32 out_fence_count;
};
```

So VII points (via `dma_descriptor.address`, a TPU virtual address previously set up with
`MAP_BUFFER`/`MAP_DMABUF`) at the compiled graph + its I/O buffers. The weight/activation matrices
live in those mapped dma-bufs; VII just says "run it." There are **8 outstanding-command credits**
per client (`EDGETPU_NUM_VII_CREDITS`), refunded as responses drain via `VII_RESPONSE`.

## Observe it yourself (rooted device)

```sh
PID=$(pgrep -f com.google.edgetpu.tachyon-service)
strace -f -tt -yy -e trace=ioctl -p "$PID" 2>&1 | grep edgetpu-soc
# you'll see, while a model runs:
#   ioctl(.. </dev/edgetpu-soc>, _IOC(.., 0xed, 0x23, 0x50), ..) = 0   ← VII_COMMAND
#   ioctl(.. </dev/edgetpu-soc>, _IOC(.., 0xed, 0x24, 0x18), ..) = 0   ← VII_RESPONSE
#   ioctl(.. </dev/edgetpu-soc>, _IOC(.., 0xed, 0x11, 0x28), ..) = 0   ← MAP_DMABUF
```

## Why this matters

The `int8/fp16` ceiling people hit is an **NNAPI API limit** (the operand-type enum), which lives
*above* VII. **VII itself is quant-agnostic** — it submits a compiled graph (DGC0) regardless of
bit-width, so it has **no int8 ceiling**. Sub-byte (int4 / int2 / ternary) graphs dispatch through
this exact same `VII_COMMAND` path; the bit-width is baked into the compiled DGC0, not the ioctl.
That's why the AOT / CCf path (see [`ccf-compiler/`](ccf-compiler/)) escapes the ceiling NNAPI
imposes.

*Device-owner reverse engineering of your own hardware, for research and learning. The ioctl table
is from the public AOSP kernel (`kernel/google-modules/edgetpu/rio`).*
