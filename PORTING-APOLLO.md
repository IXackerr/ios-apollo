# Porting ios-sweet to the Xiaomi Mi 10T / 10T Pro (apollo, SM8250)

Status: **the shim probe runs on the phone (2026-10-21); no XNU boot yet.**
Branch `apollo`.

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
| Kernel patch 0082: volume-up GPIO read from `/sweet-keys` (apollo: PM8150 GPIO6, PPID 0x0c5, base 0xc500); sweet unchanged when the node is absent | `xnu/patches/0082-…` | applies with `patch --dry-run`; syntax-checked with stub headers; **not built by CI yet** |

## Hardware map: sweet → apollo

| Item | sweet (SM7150) | apollo (SM8250) | Action |
|---|---|---|---|
| CPUs | 6×A55 + 2×A76, MPIDR Aff1 = core | 4×A55 (cpu@0–300) + 4×A77 (cpu@400–700) | same MPIDR layout, SMP patch 0078 should work as is; 16K + LSE confirmed on all 8 |
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
debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress
rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1
wdt_test=no_user_checkin wdt=-1 launchd_unsecure_cache=1
sweettouch=0 sweetusb=0
```

`sweettouch=0`: the sweet touch driver would program QUP1 SE7 and probe a
Goodix address, which is a different device on SM8250. `sweetusb=0`: the
USB probe reads the sweet QUSB2 PHY address. Start with `cpus=1`, then
`cpus=4` (A55), then `cpus=8`. Writes to userdata (`ufsrw=1 rootrw=1`)
only after the read-only boot works.

## Bring-up order

1. **Probe boot** — done (see above).
2. **Live FDT** — done: RAM window 0x1_0000_0000 + 0xfcc00000.
3. **Kernel**: fork the repo, run the `xnu` workflow on this branch
   (`gh workflow run xnu.yml --ref apollo`), build the payload with
   `mkadt.py --board apollo …` and the boot args above, boot it with
   a RAM disk first (as sweet did), then `rd=ufsroot0`.
4. **Root image**: same `tools/mkiosroot.sh` image as sweet, flashed to
   `userdata` (erases Android data; back up first).
5. **Touch driver** (NT36675 over GENI SPI), then the WLED backlight for
   display power, then the USB live log with the 7nm PHY.

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

## Needed from the phone

- `dmesg` from Android.
- `/vendor/firmware/novatek_nt36675_j3s_fw01.bin` and the GPU firmware
  (`a650_sqe.fw`, `a650_gmu.bin`, `a650_zap.*`) for later.
- Full partition backup first (`tools/apollo-backup.sh`), including
  `persist`, `modemst1/2`, `fsg`, `fsc` and `mdm1m9kefs1-3`.
