# Porting ios-sweet to the Xiaomi Mi 10T / 10T Pro (apollo, SM8250)

Status: **XNU boots on the phone to a userspace init from a RAM disk
(2026-10-21).** Branch `apollo`.

Values below come from the phone's stock boot.img DTB and live FDT where
noted, otherwise from the mainline-based apollo kernel
([royka1/apollo-linux](https://github.com/royka1/apollo-linux), branch
`apollo-6.17`: `sm8250.dtsi`, `sm8250-xiaomi-apollo.dts`, `gcc-sm8250.c`,
`pinctrl-sm8250.c`). Anything marked **verify** is still unconfirmed.

## Probe results (2026-10-21, stock DTB)

- ABL starts the shim only with the **stock DTB** (three kona SoC trees,
  board-id 0/0, the board part comes from `dtbo`) and a gzip kernel. The
  mainline apollo DTB (board-id 0x2a) gave a black screen.
- Framebuffer 0x9c000000, a8r8g8b8, 1080×2400: works.
- PSCI 1.1. CPU0-3 MIDR 0x51DF805E (Kryo 585 Silver = A55), CPU4-7
  0x411FD0D0 (A77). **16K granule + LSE on all 8 cores.**
- GICv3 works, **virtual timer INTID 27**, SGIs work.
- UFSHCI 3.0, reads work, GPT read; `userdata` starts at LBA 0x2C0000,
  ~107 GiB (Android FBE, encrypted).
- Not yet observed: the reset at the end of the probe landing in fastboot.

## Live FDT facts (pulled from recovery)

- SoC kona v2.1 (msm-id 0x164/0x20001), `dtb_idx=0`, `dtbo_idx=26`,
  board-id 0x2a after the overlay. Panel `qcom,mdss_dsi_j3s_37_02_0a_dsc_video`.
- RAM: 0x8000_0000–0xbbb0_0000, 0xc000_0000–0x1_8000_0000,
  0x1_8000_0000–0x2_8000_0000 (8 GiB). Every fixed carve-out ends below
  0xb1400000 (highest: `disp_rdump_region` 0xb0400000–0xb1400000).
  XNU window: 0x1_0000_0000 + 0xfcc00000 (same as sweet).
- ABL placed the recovery ramdisk at 0xacf96000–0xafffefdb (ends at
  0xb0000000; a bigger payload starts lower).
- `restart_reason` 0x146bf65c (`qcom,msm-imem@146bf000` + 0x65c),
  PS_HOLD 0x0c264000, `qcom,wdt@17c10000`, PSCI 1.0 node, method smc.
- SPMI PMICs: pm8150 SID 0/1, pm8150b SID 2/3, pm8150l SID 4/5, pm8009
  SID 0xa/0xb, pmxprairie (SDX55) SID 8/9. WLED `qcom,pm8150l-spmi-wled`
  at SID 5 0xd800 (+0xd900 sink).
- Touch: `spi@990000` (qcom,spi-geni, GIC SPI 0x25d) with child
  `touch_spi@0` compatible `xiaomi,spi-for-tp`. Vendor TLMM is one region
  `pinctrl@f000000`.

## What this branch already changes

| Change | Files | Checked |
|---|---|---|
| Board constants for the shim (`BOARD=apollo`): vtimer INTID 27 / ptimer 30; IMEM restart reason 0x146bf65c, PS_HOLD 0x0c264000, APSS watchdog 0x17c10000 (stock DTB) | `shim/board.h`, `shim/shim.c`, `shim/build.sh` | vtimer confirmed by the probe; reset to fastboot not yet observed |
| No-payload **hardware probe** (2 screens): MIDR + 16K-granule + LSE of all 8 cores via PSCI CPU_ON, PSCI version, GICv3 with both vtimer numberings (19 and 27), then the read-only UFS probe | `shim/shim.c` | **ran on the phone** (stock DTB) |
| `build.sh`: `BOARD=` and `DTB=` variables, no gawk dependency; apollo builds a gzip kernel with the stock os_version 14.0.0 / 2024-04 | `shim/build.sh` | the apollo ABL boots it |
| `mkadt.py --board apollo --phys-base --mem-size`: kryo585 cores (4×A55 1.8 GHz, 3×A77 2.42, 1×A77 2.84), vtimer 27, root compatible `…xiaomi,sweet|xiaomi,apollo|qcom,sm8250`, `/sweet-keys` node | `tools/mkadt.py` | sweet output byte-identical to before (all 4 flag combos) |
| `fdtmem.py`: picks the XNU RAM window from a live FDT (banks minus fixed carve-outs, prefers ≥ 4 GiB, caps at the size known to boot on sweet) | `tools/fdtmem.py` | tested on the apollo DTB with a fake 8 GiB map |
| Kernel patch 0082: volume-up GPIO read from `/sweet-keys` (apollo: PM8150 GPIO6, PPID 0x0c5, base 0xc500); sweet unchanged when the node is absent | `xnu/patches/0082-…` | on the phone: GPIO found, enabled, input |
| Kernel patch 0083: interrupt counts and GIC state before and after the IORTC wait | `xnu/patches/0083-…` | on the phone: ~100 timer interrupts per second |
| Kernel patch 0084: SweetCPU also matches `qcom,kryo585` | `xnu/patches/0084-…` | on the phone: CPU0 registers, boot reaches userspace |

## Hardware map: sweet → apollo

| Item | sweet (SM7150) | apollo (SM8250) | Action |
|---|---|---|---|
| CPUs | 6×A55 + 2×A76, MPIDR Aff1 = core | 4×A55 (cpu@0–300) + 4×A77 (cpu@400–700) | same MPIDR layout; patch 0078 brings up all 8 cores (`cpus=8`); 16K + LSE confirmed on all 8 |
| GICv3 | GICD 0x17a00000, GICR 0x17a60000 | same | none |
| Virtual timer | PPI 3 → INTID 19 | `<GIC_PPI 11>` → **INTID 27** | ADT `virtual-timer-intid` (done); confirmed by the probe |
| Timer clock | 19.2 MHz | 19.2 MHz | none |
| PSCI | smc | smc (psci-1.0) | none |
| Framebuffer | 0x9c000000, 1080×2400 a8r8g8b8 | `cont_splash_region` 0x9c000000 (0x2300000), a8r8g8b8 | confirmed by the probe |
| Panel | AMOLED, DSC video | NT36675 LCD, DSC video, 1080×2400 (`j3s_37_02_0a_dsc_video`) | CPU copy works; screen-off must also switch the **WLED backlight** off (PM8150L, SPMI SID 5, `leds@d800`; needs SPMI writes, the sweet SPMI code only reads) (patch 0079 only fades pixels, enough for AMOLED, not LCD) |
| UFS | ufshc@1d84000 | ufshc@1d84000 (UFS 3.x) | same driver; probe page 2 shows HCI version and GPT |
| SPMI arbiter | core 0x0c440000, obsrvr 0x0e600000, cnfg 0x0c40a000 | identical addresses | none |
| Power key / vol down | PM6150 PON (PPID 0x008) | PM8150 PON pwrkey / resin (PPID 0x008) | none |
| Volume up | PM6150L GPIO2 (SID 4) | **PM8150 GPIO6 (SID 0)** | patch 0082 + ADT (done) |
| RPMh | apps_rsc drv-2 0x18220000, TCS +0xd00 | identical | cmd-db locator path **verify** (sm8250 cmd-db is reserved at 0x80860000) |
| Touch | Goodix GT9896, I²C on QUP1 SE7 (0xa84000) | **Novatek NT36675 in-cell, SPI on QUP0 SE4 (spi@990000)**, 15 MHz, CS = TLMM 11 (north), reset = TLMM 38, IRQ = TLMM 39 (south) | **new driver** (below); boot with `sweettouch=0` until then |
| Touch power | RPMh vote L11C | in-cell: powered with the panel (vci = L11C 3.1–3.3 V, vddio = L14A), already on from ABL | no vote needed |
| TLMM | north tile 0x03900000 | west 0x0f100000, south 0x0f500000, north 0x0f900000; GPIO n at tile + n×0x1000 | in the touch driver |
| USB device mode | DWC3 0x0a600000, QSCRATCH 0x0a6f8800, QUSB2 PHY 0x088e2000 | DWC3 + QSCRATCH same; HS PHY is **SNPS 7nm at 0x088e3000** | boot with `sweetusb=0` until the PHY read is adapted |
| Battery | PM6150 (patch 0062) | PM8150B charger + fuel gauge | keep static battery (`sweetbattery` unset) |
| Modem | none | external SDX55 on PCIe2 (MHI); EFS in `mdm1m9kefs1-3` | out of scope; **back these partitions up** |

## Boot args for the first apollo kernel boots

```
debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress
rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1
wdt_test=no_user_checkin wdt=-1 launchd_unsecure_cache=1
sweettouch=0 sweetusb=0
```

`sweettouch=0`: the sweet touch driver would program QUP1 SE7 and probe a
Goodix address, which is a different device on SM8250. `sweetusb=0`: the
USB probe reads the sweet QUSB2 PHY address. `cpus=8` works. Writes to userdata (`ufsrw=1 rootrw=1`)
only after the read-only boot works.

## Bring-up order

1. **Probe boot** — done (see above).
2. **Live FDT** — done: RAM window 0x1_0000_0000 + 0xfcc00000.
3. **First kernel boot** — done: `rd=md0` with the static test init.
   The CPU nodes need patch 0084 (SweetCPU matched only `qcom,kryo470`;
   without it the boot thread waits for CPU registration forever).
   `cpus=4` (the A55s) and `cpus=8` (all four A77s too) bring every core up
   through PSCI CPU_ON with SGI1 verified, with patch 0078 unchanged.
4. **UFS root, read-only** — done: with `rd=ufsroot0` the kernel's UFS
   driver finds `userdata` (LUN 0, first block 0x2c0000, 109575 MiB, DMA
   buffer above 4 GiB) and, with Android's f2fs still there, HFS reports
   `unknown Volume Signature` while mountroot retries without a panic.
   The same boot shows the volume-up GPIO (PM8150 GPIO6) enabled as an
   input. Never pass `ufsrw=1` while Android data is on the partition.
5. **Root image**: same `tools/mkiosroot.sh` image as sweet, flashed to
   `userdata` (erases Android data; back up first).
6. **Touch driver** (NT36675 over GENI SPI), then the WLED backlight for
   display power, then the USB live log with the 7nm PHY.

## Backlight, battery, boot menu (2026-10-22)

- **Backlight:** the LCD is lit by the PM8150L WLED5 (SPMI SID 5,
  0xd800/0xd900), which ABL leaves configured. Patch 0090 adds PMIC writes
  through the arbiter's per-APID channel page (0x0c600000 + 0x10000 ×
  APID), whitelisted to the module enable (0xd846) and the MOD_A
  brightness and latch (0xd953/0xd954/0xd965), on peripherals owned by
  this EE only. The framebuffer switches the WLED off after its fade to
  black and back on with the display. An `AppleARMBacklight` service
  (`backlight-control`, `IODisplayParameters` with raw and milli-nit
  ranges) receives `brightness-nits` from CoreBrightness, so the iOS
  brightness slider works. Patches 0091–0093: a channel's STATUS still
  reads the previous command's DONE right after a new one is posted, so
  writes wait and retry; switching is serialized with display power; a
  brightness of 0, which CoreBrightness sends at boot before it sees the
  display on, is ignored.
- **Battery:** the PM8150B fuel gauge (FG gen4, SID 2) gives the
  monotonic SOC, voltage and current; the smb5 charger has the same
  register layout as sweet's PM6150. Charging is reported only while
  current flows into the battery (a computer's USB port may not cover the
  load without the external charge pump). USB plug and unplug are seen
  within 2 s.
- **Boot menu:** with a payload in the ramdisk the shim shows a menu for
  3 s: iOS, iOS with the kernel log, a safe mode (one CPU, stock clocks,
  no touch), reboot to fastboot or recovery, power off. Keys are read
  from the PMIC. Flashed to the `boot` partition, the phone starts iOS on
  its own; fastboot (power + volume down) and recovery (power + volume
  up) are ABL's and stay available.

## Boot time (2026-10-22)

Measured with patch 0087 (time since boot on every `exec` line, UFS totals
every 10 s), launchd starting about 11 s after the kernel:

- QuartzCore's `CAMetalContextCreate` waits up to 30 s (300 lookups,
  100 ms apart) for an `IOAcceleratorES` service before CoreAnimation
  falls back to the CPU. Patch 0089 publishes a placeholder, which names
  no Metal plug-in: the first frame now comes 14.7 s after launchd instead
  of 44.6 s, about 26 s after the kernel starts instead of 56 s.
- ABL leaves the A55s at 1516 MHz and the A77s at their lowest level
  (710 / 844 MHz). Patch 0088 sets 1804 / 1958 / 1977 MHz through EPSS
  `PERF_STATE`; the big cores stay below their 2419 / 2841 MHz maximum
  because the vendor thermal (LMh) setup does not run here.
- UFS is not the bottleneck: the device is busy for about 6 s over the
  whole boot (1.6 GiB read). The `ufscache=1 ufswait=1` options of the
  sweet driver made the early boot slower, not faster.
- Patch 0085 ends IOKitInitializeTime's 30 s IORTC wait and starts the
  calendar from the PMIC RTC.

## NT36675 touch: bare-metal probe works (2026-10-22)

`BOARD=apollo TOUCHFW=<firmware> shim/build.sh` boots a touch probe
(`shim/touch.c`) instead of the hardware probe, with the NT36675 firmware
as the ramdisk. On the phone: QUP0 v2.5, SE4 loaded as SPI with FIFO
access (16 words), DFS enabled on the SE4 clock with level 0 = TCXO
(19.2 MHz, SCLK 9.6 MHz), trim ID `.. .. .. 72 66 03`, firmware download
in ~116 ms (reset state 0xA0, then 0xA1), firmware 0x11 reporting
1080×2400, and touch reports in panel pixels with the panel's
orientation, several fingers at once. The `s_ahb` branch runs under
hardware gating and reads CLK_OFF while idle; its halt bit is not checked.

The firmware (`novatek_nt36675_j3s_fw01.bin`, 139264 bytes) is not a file
on any partition: Xiaomi's kernel builds it in (`firmware/Makefile`,
`CONFIG_TOUCHSCREEN_NT36xxx_HOSTDL_SPI`). Extract it from the phone's own
boot.img kernel; it is not part of this repository.

## NT36675 touch driver plan

- Clocks (GCC base 0x00100000): branch votes at 0x52008, bits 6 (wrap0 m-ahb),
  7 (wrap0 s-ahb), 8 (wrap0 core), 9 (wrap0 core-2x), 14 (wrap0 S4);
  S4 RCG at 0x174d0 (frequency table as `ftbl_gcc_qupv3_wrap0_s0_clk_src`:
  19.2 MHz from TCXO needs no PLL).
- GENI SE4 at 0x00990000: check the loaded protocol in `GENI_FW_REVISION_RO`
  (0x68) is SPI before using it (firmware is loaded by the boot chain).
- **Firmware download on every boot**: the IC is flash-less. The Linux driver
  (`drivers/input/touchscreen/nt36523/`, `BOOT_UPDATE_FIRMWARE`) sends
  `novatek_nt36675_j3s_fw01.bin` (from `/vendor/firmware`) over SPI after
  each reset. Use that GPL driver as protocol documentation only; write the
  XNU driver anew. Register addresses: `swrst-n8-addr` 0x03F0FE,
  `spi-rd-fast-addr` 0x03F310 (apollo DTS).
- Reports: 10 fingers, 65-byte point buffer at `EVENT_BUF_ADDR`; feed
  `sweet_hid_touch()` like the Goodix path (scale to the presented 1125×2436).

## GPU (Adreno 650)

Driver: `gpu/kernel` (kernel patch 0094), staged with `sweetgpu=N`.
Results on the phone (kernel and bare-metal shim agree):

- All bring-up stages pass: clocks, SMMU, GMU (firmware 2.1.8, HFI),
  GX power, CP with the zap shader authenticated by TrustZone (PAS 13),
  and a selftest whose ring and IB writes land and whose timestamp retires.
- SMMU: MMU-500 r2p4, 5 context banks, 6 SMRs, no stage 2. The hypervisor
  emulates the global space and keeps `SCTLR.ASIDPNE` clear in the banks.
  GPU SID 0/0x401 and GMU SID 5/0x400 get SME0/CB0 and SME1/CB1.
- The vendor `a650_sqe.fw` is version 0x93, older than the 0x95 the driver
  requires. The linux-firmware `qcom/a650_sqe.fw` (0x112) works.
- Memory the GPU, GMU and SMMU read must really be non-cacheable for the
  CPU. A write-combined `IOMemoryMap` of a wired kernel buffer is not; the
  GMU then faults on its page tables. The driver maps the pages again with
  `ml_io_map_unmappable(..., VM_WIMG_WCOMB)` (not `ml_io_map_wcomb`: its
  8 MiB submap cannot hold the GMU's 16 MiB code space).
- From iOS user space (gputest through the SweetAdreno user client, run
  by ios/sweetrun from the boot image): buffer allocation, an IB with
  CP_MEM_WRITE, timestamp 1, and 1000 submits at about 20 us each. The GPU
  still runs at its lowest level (305 MHz).
- Vulkan: Mesa turnip with the sweet kernel backend, built for iOS
  (gpu-userland workflow), runs gpu/hosttest/vktest.c on the phone:
  "Turnip Adreno (TM) 650, Vulkan 1.3", 157 device extensions, memory,
  events set by the GPU, 300 submissions with fences. It needs user-space
  reads of CTR_EL0 (patch 0096, SCTLR_EL1.UCT) for Mesa's cache flushes.
- Shaders: gpu/userland/vkcompute compiles a compute shader (ir3, 8 ms) and
  runs it over 1M elements; every result matches the CPU. 256 dependent
  integer multiply-adds per element take 17 ms (about 15.7 G per second)
  at the lowest GPU level.
- XNU's `pmap_sync_page_attributes_phys` shifted the page number in 32
  bits; on apollo, whose kernel RAM is all above 4 GiB, faulting in a
  write-combined user mapping panicked (patch 0095).
- TrustZone interrupts the zap shader's `init_image` when the system is
  busy; the call has to be resumed with the token it returns in x6.
- `shim/build.sh GPUFW=dir` runs the same driver bare metal and leaves its
  log in the recovery's pstore console, about a minute per try.

## Needed from the phone

- `dmesg` from Android.
- `/vendor/firmware/novatek_nt36675_j3s_fw01.bin` and the GPU firmware
  (`a650_sqe.fw`, `a650_gmu.bin`, `a650_zap.*`) for later.
- Full partition backup first (`tools/apollo-backup.sh`), including
  `persist`, `modemst1/2`, `fsg`, `fsc` and `mdm1m9kefs1-3`.
