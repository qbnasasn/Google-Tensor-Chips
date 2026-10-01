# NPU /dev/edgetpu-soc protocol — COMPLETE (from rio kernel source)

**Source of truth:** AOSP kernel module `kernel/google-modules/edgetpu/rio` at branch
`android-gs-caimito-6.1-android16`. Cloned to `darwinn_re/rio_kernel/`. Header
file `drivers/edgetpu/edgetpu.h` defines the entire UAPI.

**Device:** Pixel 9 Pro XL, Tensor G4 (codename **rio**, HW variant **rio_a0**).

---

## 1. Complete ioctl table (AUTHORITATIVE)

`EDGETPU_IOCTL_BASE = 0xED`. All numbers below are the full 32-bit
`_IOC*(0xED, nr, struct)` values that go in `ioctl(fd, num, arg)`.

| Number       | Real name                          | nr | Direction | Size | Purpose |
|--------------|------------------------------------|----|-----------|------|---------|
| `0xc020ed00` | `EDGETPU_MAP_BUFFER`               | 0  | RW        | 32 B | Map host VA → TPU virt addr. We called this "REGISTER_HOST". |
| `0x4020ed04` | `EDGETPU_UNMAP_BUFFER`             | 4  | W         | 32 B | Mirror of MAP_BUFFER. |
| `0x4008ed05` | `EDGETPU_SET_EVENTFD`              | 5  | W         | 8 B  | { event_id, eventfd }. event_id 0=RESPDATA, 1=FATAL_ERROR. |
| `0x4014ed06` | `EDGETPU_CREATE_GROUP`             | 6  | W         | 20 B | mailbox_attr. |
| `0x4004ed07` | `EDGETPU_JOIN_GROUP`               | 7  | W         | 4 B  | join existing group by fd. |
| `0x0000ed08` | `EDGETPU_FINALIZE_GROUP`           | 8  | —         | 0    | finalize group; firmware handshake. |
| `0x4008ed09` | `EDGETPU_SET_PERDIE_EVENTFD`       | 9  | W         | 8 B  | per-die events. |
| `0x4004ed0e` | `EDGETPU_UNSET_EVENT`              | 14 | W         | 4 B  | |
| `0x4004ed0f` | `EDGETPU_UNSET_PERDIE_EVENT`       | 15 | W         | 4 B  | |
| `0x4018ed10` | `EDGETPU_SYNC_BUFFER`              | 16 | W         | 24 B | DMA sync (cache mgmt). |
| `0xc028ed11` | `EDGETPU_MAP_DMABUF`               | 17 | RW        | 40 B | Map a dma-buf fd to TPU virt. |
| `0x4028ed12` | `EDGETPU_UNMAP_DMABUF`             | 18 | W         | 40 B | Mirror. |
| `0x4008ed13` | `EDGETPU_RELEASE_WAKE_LOCK_COMPAT` | 19 | W         | 8 B  | (compat alias) |
| `0xc020ed14` | `EDGETPU_CREATE_SYNC_FENCE`        | 20 | RW        | 32 B | Create dma_fence. |
| `0x4010ed15` | `EDGETPU_SIGNAL_SYNC_FENCE`        | 21 | W         | 16 B | |
| `0xc028ed16` | `EDGETPU_MAP_BULK_DMABUF`          | 22 | RW        | 40 B | |
| `0x4028ed17` | `EDGETPU_UNMAP_BULK_DMABUF`        | 23 | W         | 40 B | |
| `0xc018ed18` | `EDGETPU_SYNC_FENCE_STATUS`        | 24 | RW        | 24 B | |
| `0x0000ed19` | `EDGETPU_RELEASE_WAKE_LOCK`        | 25 | —         | 0    | |
| `0x0000ed1a` | `EDGETPU_ACQUIRE_WAKE_LOCK`        | 26 | —         | 0    | |
| `0x8010ed1b` | `EDGETPU_FIRMWARE_VERSION`         | 27 | R         | 16 B | reads `struct edgetpu_fw_version`. |
| `0x8008ed1c` | `EDGETPU_GET_TPU_TIMESTAMP`        | 28 | R         | 8 B  | |
| `0x8004ed1d` | `EDGETPU_GET_DEVICE_DRAM_USAGE`    | 29 | R         | 4 B  | |
| `0x4008ed1e` | `EDGETPU_ACQUIRE_EXT_MAILBOX`      | 30 | W         | 8 B  | |
| `0x4008ed1f` | `EDGETPU_RELEASE_EXT_MAILBOX`      | 31 | W         | 8 B  | |
| `0x8004ed20` | `EDGETPU_GET_FATAL_ERRORS`         | 32 | R         | 4 B  | |
| `0x4010ed22` | `EDGETPU_SET_DEVICE_PROPERTIES`    | 34 | W         | 16 B | |
| `0xc050ed23` | **`EDGETPU_VII_COMMAND`**          | 35 | RW        | 80 B | **THE submit ioctl**. struct edgetpu_vii_command_ioctl. |
| `0xc018ed24` | **`EDGETPU_VII_RESPONSE`**         | 36 | RW        | 24 B | **THE response drainer**. We previously misnamed this "POLL". |
| `0xc028ed25` | `EDGETPU_VII_LITEBUF_COMMAND`      | 37 | RW        | 40 B | Lighter submit variant. (Note: 40 B, not 80) |
| `0xc018ed26` | `EDGETPU_VII_LITEBUF_RESPONSE`     | 38 | RW        | 24 B | |

**EOPNOTSUPP** is returned by VII_COMMAND/VII_RESPONSE if either of these is false:
- `client->etdev->mailbox_manager->use_ikv` (in-kernel VII enabled)
- `client->etdev->vii_format == EDGETPU_VII_FORMAT_FLATBUFFER`

Both are true on Tensor G4.

---

## 2. The submit struct (80 bytes) — full layout

```c
// drivers/edgetpu/edgetpu.h
struct edgetpu_vii_dma_descriptor {
    __u64 address;     // TPU virtual address from MAP_BUFFER/MAP_DMABUF
    __u32 size;        // bytes
    __u32 flags;       // opaque to kernel; FW/runtime-defined
};

struct edgetpu_vii_command {
    __u64 seq;                      // sequence # (matches response.seq)
    __u16 code;                     // command type, FW/runtime-defined
    __u8  priority;                 // 0..99, lower = higher priority
    __u8  reserved_0[5];
    struct edgetpu_vii_dma_descriptor dma_descriptor;  // 16 B
    __u8  reserved_1[8];
    __u32 client_id;                // OVERRIDDEN by kernel; ignore
    __u8  qos_class;                // FW/runtime-defined
    __u8  cluster_ids_bitset;       // which clusters can handle this
    __u8  atomic;                   // execute atomically with same prio/qos
    __u8  reserved_2[1];
} __attribute__((packed));          // total: 48 B

struct edgetpu_vii_command_ioctl {
    struct edgetpu_vii_command command;     // [0..48)
    __u64 in_fence_array;                   // [48..56) userspace ptr to int[]
    __u32 in_fence_count;                   // [56..60) max 64
    __u64 out_fence_array;                  // [64..72) userspace ptr to int[]
    __u32 out_fence_count;                  // [72..76) max 64
    // [60..64) and [76..80) are alignment padding
};                                          // total: 80 B
```

All captured submits in our trace had `in_fence_count = 0` and
`out_fence_count = 0` (verified by decoding npu_replay.bin). So fence pointers
are NOT the cause of EINVAL.

---

## 3. Response struct (24 bytes)

```c
struct edgetpu_vii_response {
    __u64 seq;
    __u16 code;          // > VII_RESPONSE_CODE_KERNEL_BASE (1<<15) means kernel error
    __s8  cluster_index; // which cluster handled it (-1 if not)
    __u8  reserved;
    __u32 client_id;     // always 0 on user side
    __u64 retval;        // command-code-dependent
} __attribute__((packed));

struct edgetpu_vii_response_ioctl {
    struct edgetpu_vii_response response;
};
```

Kernel error codes (set in `response.code`):
```
VII_RESPONSE_CODE_KERNEL_CMD_TIMEOUT       = (1<<15) + 0
VII_RESPONSE_CODE_KERNEL_ENQUEUE_FAILED    = (1<<15) + 1
VII_RESPONSE_CODE_KERNEL_FENCE_ERROR       = (1<<15) + 2
VII_RESPONSE_CODE_KERNEL_FENCE_TIMEOUT     = (1<<15) + 3
VII_RESPONSE_CODE_KERNEL_CANCELED          = (1<<15) + 4   // group/firmware crashed
```

---

## 4. Credit system (the key state machine)

From `edgetpu.h`:
> `EDGETPU_NUM_VII_CREDITS 8` — max outstanding commands per client.
> Credits are consumed on enqueue, refunded when response arrives at the
> kernel level OR times out. If you submit when out of credits,
> `EDGETPU_VII_COMMAND` returns **-EBUSY**.

**This is the critical piece our v_dispatch5/6 missed.** The HAL submits a
command, then either:
- Calls `EDGETPU_VII_RESPONSE` to drain the response (refunds the credit)
- Lets the kernel timeout the command (also refunds, but slow)

We never call VII_RESPONSE in our replays. We DO call ioctl `0xc018ed24` —
but that IS `EDGETPU_VII_RESPONSE`, not POLL. So we may be right after all.

Wait — let me re-verify: in v_dispatch5/6 we treat 0xc018ed24 as a generic ioctl
and just forward whatever 24-byte payload was captured. The response struct is
24 B output, kernel populates it. So forwarding it should work.

---

## 5. EINVAL paths in `EDGETPU_VII_COMMAND` (kernel-source verified)

From `drivers/edgetpu/edgetpu-fs.c` `edgetpu_ioctl_vii_command()`:
1. `lock_check_group_member(client)` returns false → **EINVAL**
   - `client->group == NULL` (group was destroyed or never created)
2. `get_fence_array_from_user` failure → **EINVAL or EFAULT**
   - count > 64
   - copy_from_user fault on bogus user pointer
3. `edgetpu_device_group_send_vii_command` fails:
   - group not finalized → **EINVAL**
   - **group is errored** → **EINVAL** ← MOST LIKELY OUR CASE
   - group has no IOMMU domain → **EINVAL**
   - out of credits → **EBUSY** (not EINVAL)

**Hypothesis for why our submit 5+ fails:** the group becomes "errored" after
the firmware processes submits 0-4 with our half-correct buffer setup (DGCs
filled with `_post.bin` content, OUT buffers 0xAA-filled). The firmware
detects something inconsistent, signals an error, kernel marks group errored,
all subsequent submits → EINVAL.

**Verification** (when phone is back online):
```
adb shell dmesg | grep -E "edgetpu|VII|errored|fatal"
```

The log should contain a fatal-error event around the time of submit 5.

---

## 6. The full kosher submit lifecycle (what production does)

```
EDGETPU_CREATE_GROUP(mailbox_attr)
EDGETPU_FINALIZE_GROUP
EDGETPU_SET_EVENTFD(0, evfd_a)        // RESPDATA
EDGETPU_SET_EVENTFD(1, evfd_b)        // FATAL_ERROR
EDGETPU_FIRMWARE_VERSION(out)          // info only
EDGETPU_ACQUIRE_WAKE_LOCK
loop:
    EDGETPU_MAP_BUFFER(host_va, npu_va, 32B)  ← reserves NPU virt
    EDGETPU_MAP_DMABUF(fd, 40B)               ← maps actual buffers
    ...
    EDGETPU_VII_COMMAND(submit)         ← consumes 1 credit
    poll(evfd_a) until POLLIN
    EDGETPU_VII_RESPONSE(out, 24B)      ← refunds credit
    EDGETPU_UNMAP_DMABUF / UNMAP_BUFFER ← if buffer no longer needed
EDGETPU_RELEASE_WAKE_LOCK
```

The poll on `evfd_a` (the RESPDATA eventfd registered with SET_EVENTFD) is
critical — it tells you when a response is ready to drain. Without draining
responses, credits never get refunded; with credits exhausted, new submits
fail.

But more importantly: if firmware reports a fatal error, the group gets
marked errored. Every subsequent VII_COMMAND fails with EINVAL.

---

## 7. Mapping our hypothetical sub_type → required setup

The original premise was wrong — there is no per-`code` (sub_type) setup
ioctl mapping. The kernel doesn't dispatch on `code` at all. **`code` is
firmware-side dispatch**: the kernel just marshals the 80-byte command into
the firmware mailbox, and firmware consumes it.

So the real "state machine" we need is:
1. Submit credits ≤ 8 in flight
2. Group not errored
3. For each submit: drain its response before the credit pool empties

For our standalone runner this means:
- After every VII_COMMAND, poll evfd_a
- On eventfd notify, call VII_RESPONSE to drain the response
- If `response.code > (1<<15)` we got a kernel-side error; check why
- Never let in-flight count exceed 8

---

## 8. Concrete fix path for v_dispatch7

1. Properly set up SET_EVENTFD for both events (already done)
2. After every captured RIO_SUBMIT from the replay, run our own VII_RESPONSE
   drain loop (don't replay production's response payloads — let the kernel
   give us its own)
3. If a response indicates kernel error, log and stop (group is now errored,
   no recovery without a new group)
4. Cap in-flight submits at 8

Everything else stays the same as v_dispatch5 (the 44-buffer + 38-reservation
setup that already lands all addresses correctly).

---

## 9. References

- Kernel source: `darwinn_re/rio_kernel/drivers/edgetpu/edgetpu.h` (UAPI)
- ioctl handler: `darwinn_re/rio_kernel/drivers/edgetpu/edgetpu-fs.c`
- VII send/recv: `darwinn_re/rio_kernel/drivers/edgetpu/edgetpu-device-group.c`
- IKV (in-kernel VII): `darwinn_re/rio_kernel/drivers/edgetpu/edgetpu-ikv*.c`

Closing the original brief: **the "sub_type → setup-ioctl mapping" doesn't
exist** because `code` is FW-dispatch, not kernel-dispatch. The kernel state
that actually matters is (a) credit pool, (b) group-errored flag, (c) IOMMU
domain validity. All three are explicit in the kernel source.
