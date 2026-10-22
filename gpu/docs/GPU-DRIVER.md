# SweetAdreno: an Adreno 650 driver in the XNU kernel (Mi 10T Pro, SM8250)

Date: 2026-10-21. Code: `gpu/kernel/` → patch `xnu/patches/0094-apollo-adreno-gpu.patch`.
The register specs it was written from: `gpu/specs/*.md`. Every fact in a
spec carries a `file:line` reference into Linux 6.17 (apollo-linux) or the
stock kona DT. Linux (GPLv2) was used only as documentation; the code was
written from scratch (XNU is APSL).

## 1. What the driver does and why

The chain all of this exists for:

```
Metal (QuartzCore, games) → SweetMetal.bundle → metal2vulkan → Vulkan (Mesa turnip)
   → tu_knl_sweet → libsweetgpu → IOConnectCallStructMethod → SweetAdreno (kernel) → Adreno 650
```

Turnip builds the A6xx command streams (PM4) and compiles shaders (ir3) by
itself. The kernel is left with what userspace cannot do:

| Task | Why only in the kernel | Module |
|---|---|---|
| Clocks and CX GDSC | GCC/GPU_CC registers, shared across the SoC | `sweet_gpu_clk.c` |
| SMMU (MMU-500) | the GPU sees memory only through translation; page tables are physical addresses | `sweet_gpu_smmu.c` |
| GMU | the GX domain (the GPU itself) is powered on by the GMU microcontroller, not the CPU | `sweet_gpu_gmu.c` |
| Zap shader (TrustZone) | the GPU starts in secure mode and only TZ allows it to leave | `sweet_gpu_scm.c` |
| CP: SQE, ringbuffer | privileged ring buffer, CP_PROTECT, interrupts | `sweet_gpu_cp.c` |
| Memory and GPU addresses | GPU VA allocation, mapping into the SMMU and into the task | `sweet_gpu_mem.c`, `SweetAdreno.cpp` |
| UAPI | IOUserClient: contexts, buffers, submit, wait, IOSurface import | `SweetAdreno.cpp` |

### 1.1 Layers inside the driver

```
SweetAdreno.cpp         IOService + IOUserClient (C++, IOKit): UAPI, mappings, bring-up thread
SweetAdrenoDMA.cpp      DMA memory through IOBufferMemoryDescriptor (WC mapping)
sweet_gpu_os_xnu.c      the only C file that knows about XNU (log, delays, ioremap, SMC, ADT)
───────────── sgpu_os_* API (sweet_gpu.h) ─────────────
sweet_gpu_core.c        staged bring-up, sgpu_fail, sgpu_poll
sweet_gpu_clk/smmu/gmu/scm/cp/mem/cmddb.c   hardware, plain C
```

Everything below the line builds both for XNU and for Linux (`-DSGPU_HOST`).
That is why the whole GPU bring-up can be tested on a PC without the phone
(section 5).

## 2. Staged bring-up: boot-arg `sweetgpu=N`

The default is `sweetgpu=0`: the service does not start and touches no
registers at all (a test checks this). A kernel with patch 0094 behaves as if
it were not applied. `sweetgpu=N` stops after stage N. Every stage prints
what it did and what it read (`SWEET GPU: ...`), so a photo of the console or
a USB log (`sweetusb=2`) shows exactly where the hardware stopped.

| N | Stage | What happens | Success line in the log |
|---|---|---|---|
| 1 | probe | MMIO mapping, firmware references from ADT `/sweet-gpu`, clock readout (read only), SMCCC/TZ check | `scm: arm64 convention, PIL init 1 auth 1, GPU PAS 1` |
| 2 | clocks | GPLL0, GCC votes, CX GDSC, GMU RCG at 200 MHz, AHB/CX_GMU/CXO/AXI/MEMNOC/SNOC branches | `clocks on: ... cx gdsc hw 0x80000000` |
| 3 | smmu | ID registers, snapshot of the firmware's SMR/S2CR/CB, CB0←SID 0 (GPU), CB1←SID 5 (GMU) | `smmu: GPU SID 0/0x401 -> SME.. -> CB0`, `smmu: ready` |
| 4 | gmu | GMU memory in the SMMU, frequencies + ARC votes from cmd-db, RSC/PDC, load `a650_gmu.bin`, start the M3, HFI | `gmu: M3 running, INIT_RESULT 0x100`, `gmu: HFI started` |
| 5 | gx | OOB GPU_SET: the GMU powers on GX | `gmu: GX on (SPTPRAC 0 ...)` |
| 6 | cp | SQE, ringbuffer, the full hw_init (69 steps), CP_ME_INIT, zap/leaving secure mode | `zap: authenticated by TrustZone`, `cp: running` |
| 7 | selftest | a write from the ring + an IB from a "user" buffer, timestamp | `selftest: OK - ring and IB writes landed` |
| 8 | user | `SweetAdreno` is published with its user client | `SweetAdreno published` |

Other boot-args:

| boot-arg | Default | Meaning |
|---|---|---|
| `sweetgpudelay=S` | 10 | start S seconds after the service starts (a hang does not block early boot) |
| `sweetgpu_freq=N` | 1 | initial GX level (1 = 305 MHz, the lowest; Linux takes the highest) |
| `sweetgpu_smmu_force=1` | 0 | take CB0 even if the firmware left it enabled |
| `sweetgpu_coherent=1` | 0 | tell userspace the SMMU is IO-coherent (unverified, see 3.3) |
| `sweetmetal=1` | 0 | publish `MetalPluginName`/`MetalPluginClassName` (see METAL.md) |

## 3. Key decisions and the reasons for them

### 3.1 A KGSL-style UAPI, not msm DRM

`sweet_gpu_uapi.h`: 12 IOConnectCallStructMethod selectors. The KGSL model:
the kernel picks GPU addresses itself, every buffer stays in the GPU address
space until it is freed, and synchronization is a 32-bit timestamp per
context.

- The turnip backend `tu_knl_kgsl.cc` already works on this model; our
  `tu_knl_sweet.cc` is a direct descendant of it. The DRM path would need
  syncobj, sync_file, dma-buf and VM_BIND, objects XNU does not have.
- It is the same model Qualcomm uses on this SoC.
- Buffer ids are small and reused: turnip uses them to index its BO array.

### 3.2 A single GPU address space (option A from smmu-a650.md §6)

CB0, TTBR0 only, 39-bit VA, 3 levels, 4 KiB granule.

```
0x0000_0000 … 16 MiB        not mapped (NULL and the 1.125 MiB GMEM aperture, which turnip treats as reserved)
16 MiB … 256 MiB            kernel buffers: SQE, ring, memptrs, self-test (privileged)
4 GiB … 256 GiB             user buffers
0x7f_ffff_f000              UCHE trap base (never mapped)
```

Pros: no need for `CP_SMMU_TABLE_UPDATE`, the SCM aperture or protected mode
switching, and turnip gets addresses the same way it does from KGSL. Con: no
isolation between processes at the GPU level. The kernel is still protected
from user IBs: kernel buffers are mapped *privileged* (AP[1]=0). The CP reads
the ring with privilege (`CP_APRIV_CNTL=0x6e`), while user IBs execute
unprivileged and take a permission fault. This is checked by the test "user
IB cannot write privileged kernel buffers". Moving to per-process page tables
changes nothing in the UAPI (user addresses stay in the same window).

### 3.3 Memory: write-combine, as in Linux

All memory that the GPU/GMU/SMMU reads or writes is allocated through
`IOBufferMemoryDescriptor` and mapped for the CPU as Normal non-cacheable
(`kIOMapWriteCombineCache`). This is the same as `MSM_BO_WC` in Linux. The
cacheable alias of the pages is cleaned once (clean+invalidate) and never
touched again, so a dirty cache line cannot overwrite device data.

The SMMU tables live in WC memory. TCR declares a non-cacheable table walk:
the upstream DT says the SMMU is `dma-coherent`, the stock one says it is
not. Our setting is correct in both cases.

User buffers with `SGPU_MEM_CACHED` are mapped into the GPU as WB
inner-shareable. If the interconnect is coherent, the GPU snoops the CPU
cache; if not, userspace cleans the cache itself (turnip does
`DC CVAC/CIVAC`). Until `sweetgpu_coherent` is verified on hardware, turnip
sees memory as "cached, non-coherent". That is correct, just slower.

### 3.4 The SMMU under the Qualcomm hypervisor

On a retail SM8250 the SMMU global space is trapped and emulated by the QHEE
hypervisor. Hence the rules (smmu-a650.md §2.4):

- **No global reset.** Other owners' SMR/S2CR/CB are left alone, no TLBIALL*,
  no sACR writes. SMEs are looked up with `arm_smmu_find_sme` semantics, and
  CB0 is taken only if it is free.
- **Every MMIO write is a single `STR` from a real register.** `sgpu_wr` in
  `sweet_gpu.h` is inline asm with an `"r"` constraint. The GPU's S2CR value
  is exactly 0, and otherwise the compiler would emit `str wzr`, which QHEE
  corrupts (Linux has `QCOM_DUMMY_VAL` for this). `objdump` confirms that
  `wzr` stores remain only to ordinary memory.
- **SMR and S2CR are read back.** If the hypervisor filtered a write,
  bring-up stops with a clear error ("write filtered"). This is covered by a
  test as well.

### 3.5 The GMU is a mandatory middleman

GX (the GPU core) is powered by the GMU (a Cortex-M3 in the CX domain), not by
the CPU. The host loads firmware into the GMU's ITCM/DTCM and its "icache"
region. That region is allocated in 1 MiB chunks so as not to ask for 16 MiB
of contiguous memory. The host then hands the GMU the HFI queue table and
talks to it over HFI: PERF_TABLE (frequencies and RPMh ARC votes from
cmd-db), BW_TABLE, CORE_FW_START, START, GX_BW_PERF_VOTE.

The votes are built from the phone's real cmd-db (`0x80860000`). If cmd-db
does not respond, that is a fatal error: Linux ignores it, and zero votes
would switch the buses off. The frequency table depends on the speed bin
(qfprom fuse `0x784000+0x19b`): 6–8 levels from 305 to 587/670 MHz.

### 3.6 Leaving secure mode through zap

The GPU starts in secure mode. On a retail phone `RBBM_SECVID_TRUST_CNTL` is
locked by TrustZone, so a zap shader is needed. It is an ELF signed by Xiaomi,
`a650_zap`. It is placed in the carve-out at `0x8681a000` and goes through
three PAS SMCs (init_image, mem_setup, auth_and_reset, PAS id 13). After that
the CP leaves secure mode by itself with a `CP_SET_SECURE_MODE 0` packet.

TZ may refuse zap metadata above 4 GiB (XNU lives in the upper DDR bank). For
that there is an ADT property `scm-bounce` with a low buffer.

### 3.7 Submit and timestamps

For each submit the kernel appends to the ring: counters (stats), a CCU
invalidate, one `CP_INDIRECT_BUFFER` per user IB, then two `CACHE_FLUSH_TS`.
The first writes the context timestamp into its memstore slot, the second
writes the ring-wide fence with an IRQ.

`CACHE_FLUSH_TS` writes its value only after all the work above it has
finished and UCHE has been flushed. So "slot ≥ ts" means "done and visible in
memory". Completion is always read from memory (Linux does the same; the IRQ
is only a wake-up). As a result the driver works without the GPU interrupt
wired up (GIC SPI 300): waits poll with a growing pause.

### 3.8 Hangs

There is a watchdog, `sgpu_cp_hang_check`: fence and IB1 progress is checked
every 250 ms, and after 4 "strikes" without progress the GPU is considered
hung. Recovery: SQE halt, GBIF drain. All pending timestamps are declared
complete so that waits return with an error instead of hanging. Then hw_init
runs again. If that fails, the GPU stays off until reboot (submits get
ENODEV). Turnip sees this as `VK_ERROR_DEVICE_LOST` through the `hangs`
counter in GET_STATUS. Linux additionally power-cycles CX and replays the
queue; that is noted as a future improvement.

## 4. Firmware

| File (from the phone, `/vendor/firmware`) | Goes to | Read by |
|---|---|---|
| `a650_sqe.fw` | ADT `/sweet-gpu/sqe-firmware` | CP (SQE microcode), version must be ≥ 0.95 |
| `a650_gmu.bin` | `gmu-firmware` | GMU M3 (block format, parsed with bounds checks) |
| `a650_zap.mdt` + `.bNN` (or `.mbn`) | `zap-firmware` (joined by `tools/mkzap.py`) | TrustZone (PAS 13) |

`tools/mkadt.py ... --board apollo --gpu-fw DIR` adds the `/sweet-gpu` node.
The firmware is proprietary: do not put it in git; the ADT is a local build
artifact.

## 5. What is tested without the phone, and how

`make -C gpu/hosttest check` builds the **real** driver sources with
`-DSGPU_HOST` on top of a behavioral SoC model (`hosttest/model.c`). The model
itself checks the driver against the specs:

- **Power:** accessing the GMU/SMMU/RSC with CX off, or the GPU with GX off,
  is an error (on hardware it would be a bus hang).
- **SMMU:** a real walk of the driver's LPAE tables through the live
  SMR/S2CR/CB/TCR/TTBR0 registers. AP bits (privileged/read-only) are checked,
  and faults are recorded in FSR/FAR the way MMU-500 does it.
- **GMU:** before the M3 starts, the model checks RSC/PDC, chip id, ITCM,
  icache through the SMMU and the log buffer. The HFI peer parses and checks
  every message (sizes, order, 0xFAFAFAFA padding) and replies with an ack.
- **CP:** a PM4 parser with an independent check of the header parity bits,
  ring and IBs through the SMMU, privilege per `CP_APRIV_CNTL`, `CP_PROTECT`
  emulated exactly from the programmed table, secure mode, TrustZone (PAS).

13 scenarios, all passing with 0 model errors:

| Scenario | What it proves |
|---|---|
| PM4 headers = the spec table | the packet encoder |
| stage 0 | not a single register access |
| full bring-up to stage 8 | HFI order 4,3,14,10,30; 40+ registers equal the Linux values (CP_RB_CNTL, APRIV, INT mask, CP_PROTECT 0/37/47, UBWC NC_MODE, HWCG, SMMU SMR/S2CR/SCTLR/TCR/MAIR…) |
| libsweetgpu + gputest, 3000 submits | UAPI, many ring wraparounds, page fault reporting |
| user IB → kernel ring | permission fault, ring not corrupted, IB outside client memory rejected |
| hang | detection, reset, re-init, GPU works again |
| no zap / TZ locked | fallback through SECVID and a visible XPU error |
| foreign firmware SME, CB0 taken, CLIENTPD, S2CR filter | coexisting with firmware and hypervisor state |
| speed bin 3 | frequency table and `set_freq` |

Further up the stack (`gpu/hosttest/vktest.c`, `docs/METAL.md` §6): the real
Mesa turnip (`-Dfreedreno-kmds=sweet`) on top of the same driver and model.
`vulkaninfo`: "Turnip Adreno (TM) 650, Vulkan 1.3". Device creation, events
written by the GPU (`CP_MEM_WRITE` and `RB_DONE_TS`), 300 submits: 41,252
real turnip packets, not a single `CP_PROTECT` violation.

Compilation: the modules are clean under XNU's exact warning set
(`-Weverything -Werror` from `MakeInc.def`, clang 18 + per-object
`-Wno-unsafe-buffer-usage`) and under `aarch64-linux-gnu-gcc -Werror`. The
three XNU glue files (`SweetAdreno.cpp`, `SweetAdrenoDMA.cpp`,
`sweet_gpu_os_xnu.c`) are built only by the kernel CI. APIs were checked
against the xnu-8796.141.3 + 0001–0093 tree, includes against neighboring
drivers (`SweetCPU.cpp`, `sweet_ufs.c`).

**What the model does not know** (only testable on the phone): real timings,
the behavior of the specific GMU/SQE/TZ firmware, exact SMMU ID values, the
state ABL leaves behind, shader execution.

## 6. Plan on the phone (one stage at a time)

Each step is a `fastboot boot` of an image with a new boot-arg. Nothing is
flashed.

1. `sweetgpu=1`. Look at the `gcc:`/`gpucc:`/`scm:` lines. If `SCM: 64-bit
   SMCCC convention not detected`, the problem is in SMC (compare with PSCI
   from 0065).
2. `sweetgpu=2`. If "clock X stuck off": compare with `/sys/kernel/debug/clk`
   under Linux (pmOS) on the same phone.
3. `sweetgpu=3`. The most informative step for the SMMU. The log will have
   the firmware's SME/CB snapshot, `ID0/ID1/ID2/ID7`, `sCR0`. If "CB0 is in
   use by firmware": send the log and decide whether `sweetgpu_smmu_force=1`
   is safe.
4. `sweetgpu=4`. If `INIT_RESULT` times out: send `FW_BUSY`/`AO status` from
   the log. On an HFI error: the message number and code.
5. `sweetgpu=5`. GX is on, the first GPU register has been read.
6. `sweetgpu=6`. Zap: on a PAS error send the x1 code. If `CP_ME_INIT: ring
   did not drain`: the `CP_*` dump in the log.
7. `sweetgpu=7`. Self-test, the first real GPU work.
8. `sweetgpu=8` → `gputest` (and `gputest -n 1000 -f`) from userland.

If it hangs with no log: `sweetgpudelay=30` and the USB log.

## 7. Known limitations and next steps

- GPU/GMU/SMMU IRQs are not wired up (polling). Next step: GIC SPI 300
  (GPU), 304/305 (HFI/GMU), 672/673 + 678/679 (SMMU) through
  `IOInterruptEventSource`.
- No DCVS (the frequency is fixed by a boot-arg) and no DDR bandwidth
  voting: mainline does not have it either, see gmu-hfi-a650.md §7.2-1.
- No power collapse (GX/CX stay on after bring-up).
- Per-process page tables (`CP_SMMU_TABLE_UPDATE` + SCM aperture), once
  isolation is needed.
- IOSurface import in turnip (`bo_init_dmabuf` by SweetSurface id), needed
  for Metal rendering into CA layers (METAL.md §5.3).
