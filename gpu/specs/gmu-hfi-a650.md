# Adreno 650 (SM8250) GMU / RPMh / HFI bring-up specification

Target: Adreno 650, chip id `0x06050002`, `ADRENO_6XX_GEN3`, on SM8250 (Xiaomi Mi 10T Pro "apollo").
Source of truth: the Linux tree at `apollo-linux` (royka1/apollo-linux, branch apollo-6.17) (paths below are relative to
`drivers/gpu/drm/msm/adreno/` unless given in full). This document covers only the code path Linux
takes for A650. It leaves out the a7xx, a610/a619 "GMU wrapper", and legacy a630/a618 (HFI v1) paths
except where it helps to contrast them.

Notation:
* `GMU[x]` is a register at dword offset `x` from the GMU MMIO base `0x03D6A000`. The physical address is `0x03D6A000 + 4*x`.
* `RSCC[x]` is `0x03DE0000 + 4*x`. `PDC[x]` is `0x0B290000 + 4*x`. `GPU[x]` is `0x03D00000 + 4*x`.
* "poll(R, cond, 100us, 10ms)" uses Linux `readl_poll_timeout(addr, val, cond, 100, 10000)`. It reads, sleeps about 100 µs, and gives up after 10 ms.
* Every register is 32-bit and accessed with plain 32-bit MMIO (`readl`/`writel`).

---------------------------------------------------------------------------------------------------

## 0. Key A650 facts (capability flags and quirks as Linux evaluates them)

| Property | A650 value | Where |
|---|---|---|
| family | `ADRENO_6XX_GEN3`, so `adreno_is_a650_family()` is **true** (GEN3 or GEN4) | `a6xx_catalog.c:963-988`, `adreno_gpu.h:495-501` |
| `adreno_is_a650()` (revn 650) | true. It selects `a650_build_bw_table` | `adreno_gpu.h:429-432`, `a6xx_hfi.c:744-745` |
| `adreno_is_a660_family()` | **false**: no "debug" BO, dummy BO is 4 KiB | `a6xx_gmu.c:1956-1965` |
| `adreno_is_a7xx()` / `a740_family` | false | |
| `gmu->legacy` | **false**. It is set only for a630 family (`a6xx_gmu.c:1996-1998`) and GMU-wrapper (`:1871`). So: HFI v2, "new" OOB bits, no SPTPRAC control by the CPU, no BOOT_SLUMBER/GX_VOTE_IDX handshake | |
| `gmu->idle_level` | `GMU_IDLE_STATE_ACTIVE` (0): no IFPC, no SPTP power collapse | `a6xx_gmu.c:1936`, `a6xx_gmu.h:51-57` |
| boot type | **always COLD**. Warm boot is disabled for non-legacy a6xx | `a6xx_gmu.c:1104-1110` |
| `info->a6xx->bcms` | NULL: no dynamic DDR BW table, static `a650_build_bw_table` | `a6xx_catalog.c:977-982` |
| `info->a6xx->gmu_chipid` | 0: chip id is computed (gives **0x06050200**, see §1.6) | `a6xx_gmu.c:890-903` |
| `gmu_cgc_mode` | `0x00020202` (used by GPU hw_init, see §1.9) | `a6xx_catalog.c:980` |
| quirks | `ADRENO_QUIRK_HAS_CACHED_COHERENT \| ADRENO_QUIRK_HAS_HW_APRIV` (no GMU effect) | `a6xx_catalog.c:973-974` |
| firmware | `a650_gmu.bin` (GMU), `a650_sqe.fw` (CP), `a650_zap.mdt` | `a6xx_catalog.c:967-976` |
| PDC sequencer location | "PDC in AOP" (`pdc_in_aop = true`). The host does **not** load the PDC sequencer or PDC TCS commands, and does not map `gmu_pdc_seq` | `a6xx_gmu.c:580-594, 633-634` |
| RSCC mapping | separate region `rscc` = `0x03DE0000` (not `gmu+0x23000`) | `a6xx_gmu.c:2023-2032` |
| ITCM/DTCM base (GMU view) | ITCM `0x00000000`, DTCM `0x10004000`, each 16 KiB | `a6xx_gmu.c:770-774` |
| ACD | not used by mainline (no `qcom,opp-acd-level` in sm8250.dtsi and no `qcom,qmp` in the gmu node). The stock kernel **does** use ACD | `a6xx_gmu.c:1668-1735` |
| `qmp` | absent in the gmu DT node, so `gmu->qmp` is an ERR_PTR. Harmless because ACD is not requested | `a6xx_gmu.c:2056-2060` |

### 0.1 Register base mapping (verified)

`a6xx_gmu_get_mmio(pdev, "gmu")` ioremaps the DT `gmu` resource (`0x03D6A000`, size `0x30000`)
verbatim, with no subtraction (`a6xx_gmu.c:1755-1774, 2017`). `gmu_read`/`gmu_write` take a **dword**
offset and do `mmio + (offset << 2)` (`a6xx_gmu.h:122-130`). The XML offsets in
`registers/adreno/a6xx_gmu.xml` are therefore dword offsets relative to `0x03D6A000`. They are the
downstream/KGSL GPU-relative dword offsets minus `0x1A800`, because `0x03D6A000 = 0x03D00000 + 4*0x1A800`.
Example: KGSL `A6XX_GMU_CM3_ITCM_START = 0x1B400`, and `0x1B400 - 0x1A800 = 0xC00` = XML.

Cross-check: `GPU_CC_GX_GDSCR` XML `0x9C03` maps to `0x03D6A000 + 0x2700C = 0x03D9100C`. That is
gpucc (`0x03D90000`) + `0x100C`, the real GX GDSCR. The mapping is consistent.

RSCC registers: the XML comment says "starts at offset 0x8c00 on most gpus" (`a6xx_gmu.xml`, before
`GPU_RSCC_RSC_STATUS0_DRV0`). That is the old `gmu->mmio + 0x23000` aliasing (`a6xx_gmu.c:2031`). On
A650 they are relative to the separate `rscc` mapping at `0x03DE0000`. Accessors: `gmu_read_rscc`/`gmu_write_rscc`, also `offset << 2` (`a6xx_gmu.h:162-174`).

PDC registers (`A6XX_PDC` domain, `a6xx.xml:3721-3748`) are dword offsets relative to the `gmu_pdc`
mapping `0x0B290000`, written with `pdc_write(ptr, off, v) = writel(v, ptr + (off << 2))` (`a6xx_gmu.c:558-561`).

### 0.2 Register address table (every register used in this spec)

| Name | Base | dword off | byte off | Physical |
|---|---|---|---|---|
| GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL | GMU | 0x0080 | 0x00200 | 0x03D6A200 |
| GMU_GX_SPTPRAC_POWER_CONTROL (legacy only) | GMU | 0x0081 | 0x00204 | 0x03D6A204 |
| GMU_CM3_ITCM_START (16 KiB window) | GMU | 0x0C00 | 0x03000 | 0x03D6D000 to 0x03D70FFF |
| GMU_CM3_DTCM_START (16 KiB window) | GMU | 0x1C00 | 0x07000 | 0x03D71000 to 0x03D74FFF |
| GMU_BOOT_SLUMBER_OPTION (legacy only, in DTCM) | GMU | 0x23F8 | 0x08FE0 | 0x03D72FE0 |
| GMU_GX_VOTE_IDX / GMU_MX_VOTE_IDX (legacy only) | GMU | 0x23F9/0x23FA | | 0x03D72FE4/0x03D72FE8 |
| GMU_DCVS_ACK_OPTION/PERF_SETTING/BW_SETTING/RETURN (legacy only) | GMU | 0x23FC..0x23FF | | 0x03D72FF0..0x03D72FFC |
| GMU_CORE_FW_VERSION (= DTCM_START+0xFF8, i.e. DTCM byte 0x3FE0) | GMU | 0x2BF8 | 0x0AFE0 | 0x03D74FE0 |
| GMU_ICACHE_CONFIG | GMU | 0x4C00 | 0x13000 | 0x03D7D000 |
| GMU_DCACHE_CONFIG | GMU | 0x4C01 | 0x13004 | 0x03D7D004 |
| GMU_SYS_BUS_CONFIG | GMU | 0x4C0F | 0x1303C | 0x03D7D03C |
| GMU_CM3_SYSRESET | GMU | 0x5000 | 0x14000 | 0x03D7E000 |
| GMU_CM3_BOOT_CONFIG | GMU | 0x5001 | 0x14004 | 0x03D7E004 |
| GMU_CM3_FW_BUSY | GMU | 0x501A | 0x14068 | 0x03D7E068 |
| GMU_CM3_FW_INIT_RESULT | GMU | 0x501C | 0x14070 | 0x03D7E070 |
| GMU_CM3_CFG | GMU | 0x502D | 0x140B4 | 0x03D7E0B4 |
| GMU_CX_GMU_POWER_COUNTER_ENABLE | GMU | 0x5040 | 0x14100 | 0x03D7E100 |
| GMU_CX_GMU_POWER_COUNTER_SELECT_0 | GMU | 0x5041 | 0x14104 | 0x03D7E104 |
| GMU_CX_GMU_POWER_COUNTER_XOCLK_0_L/H | GMU | 0x5044/0x5045 | | 0x03D7E110/0x03D7E114 |
| GMU_PWR_COL_INTER_FRAME_CTRL | GMU | 0x50C0 | 0x14300 | 0x03D7E300 |
| GMU_PWR_COL_INTER_FRAME_HYST | GMU | 0x50C1 | 0x14304 | 0x03D7E304 |
| GMU_PWR_COL_SPTPRAC_HYST | GMU | 0x50C2 | 0x14308 | 0x03D7E308 |
| GMU_GMU_PWR_COL_KEEPALIVE | GMU | 0x50C3 | 0x1430C | 0x03D7E30C |
| GMU_SPTPRAC_PWR_CLK_STATUS | GMU | 0x50D0 | 0x14340 | 0x03D7E340 |
| GMU_RPMH_CTRL | GMU | 0x50E8 | 0x143A0 | 0x03D7E3A0 |
| GPU_GMU_CX_GMU_RPMH_POWER_STATE | GMU | 0x50EC | 0x143B0 | 0x03D7E3B0 |
| GPU_GMU_CX_GMU_CX_FAL_INTF | GMU | 0x50F0 | 0x143C0 | 0x03D7E3C0 |
| GPU_GMU_CX_GMU_CX_FALNEXT_INTF | GMU | 0x50F1 | 0x143C4 | 0x03D7E3C4 |
| GPU_GMU_CX_GMU_PWR_COL_CP_MSG | GMU | 0x5100 | 0x14400 | 0x03D7E400 |
| GPU_GMU_CX_GMU_PWR_COL_CP_RESP | GMU | 0x5101 | 0x14404 | 0x03D7E404 |
| GMU_HFI_CTRL_STATUS | GMU | 0x5180 | 0x14600 | 0x03D7E600 |
| GMU_HFI_VERSION_INFO | GMU | 0x5181 | 0x14604 | 0x03D7E604 |
| GMU_HFI_SFR_ADDR | GMU | 0x5182 | 0x14608 | 0x03D7E608 |
| GMU_HFI_QTBL_INFO | GMU | 0x5184 | 0x14610 | 0x03D7E610 |
| GMU_HFI_QTBL_ADDR | GMU | 0x5185 | 0x14614 | 0x03D7E614 |
| GMU_HFI_CTRL_INIT | GMU | 0x5186 | 0x14618 | 0x03D7E618 |
| GMU_GMU2HOST_INTR_SET | GMU | 0x5190 | 0x14640 | 0x03D7E640 |
| GMU_GMU2HOST_INTR_CLR | GMU | 0x5191 | 0x14644 | 0x03D7E644 |
| GMU_GMU2HOST_INTR_INFO | GMU | 0x5192 | 0x14648 | 0x03D7E648 |
| GMU_GMU2HOST_INTR_MASK | GMU | 0x5193 | 0x1464C | 0x03D7E64C |
| GMU_HOST2GMU_INTR_SET | GMU | 0x5194 | 0x14650 | 0x03D7E650 |
| GMU_HOST2GMU_INTR_CLR | GMU | 0x5195 | 0x14654 | 0x03D7E654 |
| GMU_HOST2GMU_INTR_RAW_INFO | GMU | 0x5196 | 0x14658 | 0x03D7E658 |
| GMU_GENERAL_7 | GMU | 0x51CC | 0x14730 | 0x03D7E730 |
| GMU_AO_INTERRUPT_EN | GMU | 0x9303 | 0x24C0C | 0x03D8EC0C |
| GMU_AO_HOST_INTERRUPT_CLR | GMU | 0x9304 | 0x24C10 | 0x03D8EC10 |
| GMU_AO_HOST_INTERRUPT_STATUS | GMU | 0x9305 | 0x24C14 | 0x03D8EC14 |
| GMU_AO_HOST_INTERRUPT_MASK | GMU | 0x9306 | 0x24C18 | 0x03D8EC18 |
| GMU_RSCC_CONTROL_REQ | GMU | 0x9307 | 0x24C1C | 0x03D8EC1C |
| GMU_RSCC_CONTROL_ACK | GMU | 0x9308 | 0x24C20 | 0x03D8EC20 |
| GPU_GMU_AO_GMU_CGC_MODE_CNTL | GMU | 0x9309 | 0x24C24 | 0x03D8EC24 |
| GPU_GMU_AO_GMU_CGC_DELAY_CNTL | GMU | 0x930A | 0x24C28 | 0x03D8EC28 |
| GPU_GMU_AO_GMU_CGC_HYST_CNTL | GMU | 0x930B | 0x24C2C | 0x03D8EC2C |
| GPU_GMU_AO_GPU_CX_BUSY_STATUS | GMU | 0x930C | 0x24C30 | 0x03D8EC30 |
| GPU_GMU_AO_GPU_CX_BUSY_STATUS2 | GMU | 0x930D | 0x24C34 | 0x03D8EC34 |
| GPU_GMU_AO_GPU_CX_BUSY_MASK | GMU | 0x930E | 0x24C38 | 0x03D8EC38 |
| GMU_AO_AHB_FENCE_CTRL | GMU | 0x9310 | 0x24C40 | 0x03D8EC40 |
| GMU_AHB_FENCE_RANGE_0 | GMU | 0x9311 | 0x24C44 | 0x03D8EC44 |
| GMU_AHB_FENCE_STATUS | GMU | 0x9313 | 0x24C4C | 0x03D8EC4C |
| GMU_AHB_FENCE_STATUS_CLR | GMU | 0x9314 | 0x24C50 | 0x03D8EC50 |
| GPU_CC_GX_GDSCR | GMU | 0x9C03 | 0x2700C | 0x03D9100C |
| GPU_RSCC_RSC_STATUS0_DRV0 | RSCC | 0x0004 | 0x0010 | 0x03DE0010 |
| RSCC_PDC_SEQ_START_ADDR | RSCC | 0x0008 | 0x0020 | 0x03DE0020 |
| RSCC_PDC_MATCH_VALUE_LO | RSCC | 0x0009 | 0x0024 | 0x03DE0024 |
| RSCC_PDC_MATCH_VALUE_HI | RSCC | 0x000A | 0x0028 | 0x03DE0028 |
| RSCC_PDC_SLAVE_ID_DRV0 | RSCC | 0x000B | 0x002C | 0x03DE002C |
| RSCC_HIDDEN_TCS_CMD0_ADDR (+2n = CMDn ADDR) | RSCC | 0x000D | 0x0034 | 0x03DE0034 |
| RSCC_HIDDEN_TCS_CMD0_DATA (+2n = CMDn DATA) | RSCC | 0x000E | 0x0038 | 0x03DE0038 |
| RSCC_OVERRIDE_START_ADDR | RSCC | 0x0100 | 0x0400 | 0x03DE0400 |
| RSCC_SEQ_BUSY_DRV0 | RSCC | 0x0101 | 0x0404 | 0x03DE0404 |
| RSCC_SEQ_MEM_0_DRV0 (+0..+4) | RSCC | 0x0180 | 0x0600 | 0x03DE0600..0x03DE0610 |
| RSCC_TCS0_DRV0_STATUS | RSCC | 0x0346 | 0x0D18 | 0x03DE0D18 |
| RSCC_TCS1_DRV0_STATUS | RSCC | 0x03EE | 0x0FB8 | 0x03DE0FB8 |
| RSCC_TCS2_DRV0_STATUS | RSCC | 0x0496 | 0x1258 | 0x03DE1258 |
| RSCC_TCS3_DRV0_STATUS | RSCC | 0x053E | 0x14F8 | 0x03DE14F8 |
| PDC_GPU_ENABLE_PDC | PDC | 0x1140 | 0x4500 | 0x0B294500 |
| PDC_GPU_SEQ_START_ADDR | PDC | 0x1148 | 0x4520 | 0x0B294520 |
| RBBM_GBIF_HALT / _ACK | GPU | 0x0016/0x0017 | | 0x03D00058/0x03D0005C |
| RBBM_SW_RESET_CMD | GPU | 0x0043 | | 0x03D0010C |
| GBIF_HALT / GBIF_HALT_ACK | GPU | 0x3C45/0x3C46 | | 0x03D0F114/0x03D0F118 |

Bitfields used (from `a6xx_gmu.xml`):
* `GMU2HOST_INTR_INFO`: MSGQ = bit 0, CM3_FAULT = bit 23. OOB acks are in bits 24-31.
* `AO_HOST_INTERRUPT_STATUS`: WDOG_BITE = bit 0, RSCC_COMP = 1, VDROOP = 2, FENCE_ERR = 3, DBD_WAKEUP = 4, HOST_AHB_BUS_ERROR = 5.
* `GMU_RPMH_CTRL`: RPMH_INTERFACE_ENABLE = 0, LLC_VOTE_ENABLE = 4, DDR_VOTE_ENABLE = 8, MX_VOTE_ENABLE = 9, CX_VOTE_ENABLE = 10, GFX_VOTE_ENABLE = 11.
* `GMU_PWR_COL_INTER_FRAME_CTRL`: IFPC_ENABLE = 0, HM_POWER_COLLAPSE_ENABLE = 1, SPTPRAC_POWER_CONTROL_ENABLE = 2.
* `GMU_SPTPRAC_PWR_CLK_STATUS`: GX_HM_GDSC_POWER_OFF = bit 6, GX_HM_CLK_OFF = bit 7 (used by `a6xx_gmu_gx_is_on`, `a6xx_gmu.c:94-107`).
* `GPU_CX_BUSY_STATUS`: GPUBUSYIGNAHB = bit 23.

### 0.3 Resources from DT (`arch/arm64/boot/dts/qcom/sm8250.dtsi:3033-3071`)

* reg `gmu` 0x03D6A000/0x30000, `rscc` 0x03DE0000/0x10000, `gmu_pdc` 0x0B290000/0x10000, `gmu_pdc_seq` 0x0B490000/0x10000. On A650 Linux never touches `gmu_pdc_seq`.
* IRQs: `hfi` = GIC SPI 304 (INTID 336), `gmu` = GIC SPI 305 (INTID 337), both level-high. Linux requests them with `IRQF_TRIGGER_HIGH | IRQF_NO_AUTOEN` (`a6xx_gmu.c:1776-1791`).
* clocks: `ahb` = GPU_CC_AHB_CLK, `gmu` = GPU_CC_CX_GMU_CLK, `cxo` = GPU_CC_CXO_CLK, `axi` = GCC_DDRSS_GPU_AXI_CLK, `memnoc` = GCC_GPU_MEMNOC_GFX_CLK. There is no `hub` clock, so `gmu->hub_clk` is NULL and `clk_set_rate(hub, 150 MHz)` is a no-op (`a6xx_gmu.c:1080-1081, 1749-1750`).
* power-domains: `cx` = GPU_CX_GDSC, `gx` = GPU_GX_GDSC.
* iommus: `<&adreno_smmu 5 0x400>`: SID 5, mask 0x400 (also matches SID 0x405).
* gmu OPP: a single 200 MHz entry at `RPMH_REGULATOR_LEVEL_MIN_SVS` (48).
* The stock DT (`kona-v21.dts (decompiled stock DTB):24364-24397`) has the same gmu/pdc/pdc_seq regs and the same IRQs (0x130/0x131). Its `rscc` is attached to the kgsl-3d0 node (`:23753`). It has two GMU SMMU CBs: `gmu_user` SID 4 and `gmu_kernel` SID 5 (`:24384-24396`). Mainline uses only SID 5.

---------------------------------------------------------------------------------------------------

## 1. Ordered bring-up: "GPU off" to "GMU running, HFI started, GPU ready for hw_init"

### 1.0 Prerequisites (owned by the clock/power spec)

Linux `a6xx_gmu_resume()` (`a6xx_gmu.c:1055-1141`) assumes or does the following first:

1. **GPU_CX_GDSC on.** This is `pm_runtime_get_sync(gmu->dev)`, which powers `cxpd` through the device link (`a6xx_gmu.c:1068, 2043-2053`).
   * The Adreno SMMU (0x03DA0000) is in the CX domain (`sm8250.dtsi:3108`). Its context bank for SID 5 must be valid before the GMU fetches from the icache/HFI/log BOs. Linux's arm-smmu driver restores SMMU state on runtime resume. **An XNU driver must (re)program the SID-5 context bank after every CX GDSC power-up.**
2. **GX genpd "get".** This is `pm_runtime_get_sync(gmu->gxpd)` (`a6xx_gmu.c:1075-1076`). It is refcount only. The comment says it "won't actually do anything": the mainline gpucc GX GDSC uses a do-nothing enable callback (`gdsc_gx_do_nothing_enable` in `drivers/clk/qcom/gdsc.c`; that file is not in this tree, so this is from memory). **The GMU firmware powers GX itself (RPMh GFX rail vote plus GX GDSC). The host must not turn GX on.** The host only forces GX off on the failure or teardown path (§6).
3. **Clocks.** `clk_set_rate(GPU_CC_CX_GMU_CLK, 200000000)`. There is no hub clock on A650. Then enable all five GMU clocks: AHB, CX_GMU (200 MHz), CXO (19.2 MHz), GCC_DDRSS_GPU_AXI, GCC_GPU_MEMNOC_GFX (`a6xx_gmu.c:1079-1082`).
4. **Bus vote.** `a6xx_gmu_set_initial_bw()` calls `dev_pm_opp_set_opp(gpu_dev, opp[current_perf_index])` (`a6xx_gmu.c:1042-1053, 1090`). The sm8250 GPU node has no `interconnects` and no power domain, so **this is a no-op on SM8250**. Nobody on the host votes GPU DDR bandwidth. See §7 (risk).

Done once at probe, not on every resume: firmware fetch, BO allocation and SMMU mapping (§2), OPP/vote tables (§3.5), HFI queue table init (§3.1), and **`a6xx_gmu_rpmh_init()` (§1.2)**.

### 1.1 Probe-time order (`a6xx_gmu_init`, `a6xx_gmu.c:1918-2115`)

1. `idle_level = ACTIVE` (`:1936`). Get clocks (`:1941`).
2. Create the GMU IOMMU domain and a 2 GiB VM `[0, 0x80000000)` (`a6xx_gmu_memory_probe`, `:1316-1331`).
3. Allocate BOs in this order: dummy (4 KiB @ 0x60000000), icache (16 MiB − 16 KiB @ 0x4000), log (16 KiB, floating), hfi (16 KiB, floating) (`:1956-2014`). See §2.
4. Map `gmu` and `rscc` (`:2017-2032`). Request `hfi` and `gmu` IRQs, left disabled (`:2035-2036`).
5. Attach the `cx` pd plus device link (`:2043-2053`). Get qmp, which fails on mainline sm8250 (`:2056`). Attach the `gx` pd (`:2070`).
6. `a6xx_gmu_pwrlevels_probe()` builds the frequency tables and ARC votes using cmd-db (`:2073`, §3.5). **Its return value is ignored**: if cmd-db fails, the vote words stay zero and probe continues silently.
7. `a6xx_gmu_acd_probe()`: no ACD on mainline (`:2075`).
8. `a6xx_hfi_init()` writes the queue table into the hfi BO (`:2080`, §3.1).
9. `a6xx_gmu_rpmh_init()` (`:2083`, §1.2).

> RISK: Linux runs `a6xx_gmu_rpmh_init()` at probe **without** an explicit `pm_runtime_get` of the GMU, so without explicitly turning on CX or the GMU clocks. It writes RSCC and PDC and also touches `GMU_RSCC_CONTROL_REQ` (GMU AO space) in `a6xx_rpmh_stop`. It works on Linux only because these blocks are reachable at probe time (bootloader state or AO domain). **XNU: run §1.2 only with CX GDSC on and the GMU clocks enabled.** Linux never redoes it, which implies the RSCC/PDC programming survives CX collapse (AO domain). Re-running it before each cold boot is plausible but untested by Linux.

### 1.2 `a6xx_gmu_rpmh_init()`: A650 branch (`a6xx_gmu.c:566-691`)

`pdc_in_aop = true` because `adreno_is_a650_family` (`:580-582`). Exact order:

| # | Access | Address | Value | Source |
|---|---|---|---|---|
| 1 | W RSCC GPU_RSCC_RSC_STATUS0_DRV0 ("Disable SDE clock gating") | 0x03DE0010 | 0x01000000 (BIT 24) | :597 |
| 2 | W RSCC_PDC_SLAVE_ID_DRV0 | 0x03DE002C | 0x00000001 | :600 |
| 3 | W RSCC_HIDDEN_TCS_CMD0_DATA | 0x03DE0038 | 0x00000000 | :601 |
| 4 | W RSCC_HIDDEN_TCS_CMD0_ADDR | 0x03DE0034 | 0x00000000 | :602 |
| 5 | W RSCC_HIDDEN_TCS_CMD0_DATA+2 (CMD1 DATA) | 0x03DE0040 | 0x00000000 | :603 |
| 6 | W RSCC_HIDDEN_TCS_CMD0_ADDR+2 (CMD1 ADDR) | 0x03DE003C | 0x00000000 | :604 |
| 7 | W RSCC_HIDDEN_TCS_CMD0_DATA+4 (CMD2 DATA) | 0x03DE0048 | **0x80000000** (a740 would use 0x80000021) | :605-606 |
| 8 | W RSCC_HIDDEN_TCS_CMD0_ADDR+4 (CMD2 ADDR) | 0x03DE0044 | 0x00000000 | :607 |
| 9 | W RSCC_OVERRIDE_START_ADDR | 0x03DE0400 | 0x00000000 | :608 |
| 10 | W RSCC_PDC_SEQ_START_ADDR | 0x03DE0020 | 0x00004520 (= PDC byte offset of GPU_SEQ_START_ADDR) | :609 |
| 11 | W RSCC_PDC_MATCH_VALUE_LO | 0x03DE0024 | 0x00004510 | :610 |
| 12 | W RSCC_PDC_MATCH_VALUE_HI | 0x03DE0028 | 0x00004514 | :611 |
| 13 | W RSCC_SEQ_MEM_0_DRV0+0 | 0x03DE0600 | **0xEAAAE5A0** | :620 |
| 14 | W RSCC_SEQ_MEM_0_DRV0+1 | 0x03DE0604 | **0xE1A1EBAB** | :621 |
| 15 | W RSCC_SEQ_MEM_0_DRV0+2 | 0x03DE0608 | **0xA2E0A581** | :622 |
| 16 | W RSCC_SEQ_MEM_0_DRV0+3 | 0x03DE060C | **0xECAC82E2** | :623 |
| 17 | W RSCC_SEQ_MEM_0_DRV0+4 | 0x03DE0610 | **0x0020EDAD** | :624 |
| — | (pre-650 PDC sequencer and TCS1/TCS3 programming, `:636-674`, is **skipped**: `goto setup_pdc`) | | | :633-634 |
| 18 | W PDC_GPU_SEQ_START_ADDR | 0x0B294520 | 0x00000000 | :678 |
| 19 | W PDC_GPU_ENABLE_PDC | 0x0B294500 | 0x80000001 | :679 |
| 20 | `wmb()` | | | :682 |
| 21 | `a6xx_rpmh_stop()` (below) | | | :684 |
| 22 | unmap `gmu_pdc` | | | :686-690 |

The non-A650 values are listed for contrast only. Pre-650 RSC sequencer: 0xa7a506a0, 0xa1e6a6e7, 0xa2e081e1, 0xe9a982e2, 0x0020e8a8.

**`a6xx_rpmh_stop()`** (`a6xx_gmu.c:543-556`):
1. W GMU_RSCC_CONTROL_REQ (0x03D8EC1C) = 0x1.
2. poll RSCC GPU_RSCC_RSC_STATUS0_DRV0 (0x03DE0010) until `val & BIT(16)`, 100 µs interval, 10 ms timeout. On timeout it logs "Unable to power off the GPU RSC" and continues.
3. W GMU_RSCC_CONTROL_REQ = 0x0.

### 1.3 `a6xx_gmu_resume()`: register-level order (`a6xx_gmu.c:1055-1141`)

After §1.0:

| # | Action | Address | Value / condition | Source |
|---|---|---|---|---|
| R1 | W GMU_AO_HOST_INTERRUPT_CLR | 0x03D8EC10 | 0xFFFFFFFF | :1093 |
| R2 | W GMU_AO_HOST_INTERRUPT_MASK | 0x03D8EC18 | ~(BIT0\|BIT3\|BIT5) = **0xFFFFFFD6** (unmask WDOG_BITE, FENCE_ERR, HOST_AHB_BUS_ERROR) | :1094, :950-953 |
| R3 | enable the `gmu` IRQ (SPI 305) | | | :1095 |
| R4 | boot type is COLD (A650 never warm) | | | :1104-1110 |
| R5 | `a6xx_gmu_fw_start(gmu, GMU_COLD_BOOT)` (§1.4) | | | :1112 |
| R6 | `a6xx_hfi_start(gmu, GMU_COLD_BOOT)` (§3.4) | | | :1116 |
| R7 | W GMU_GMU2HOST_INTR_CLR | 0x03D7E644 | 0xFFFFFFFF | :1124 |
| R8 | W GMU_GMU2HOST_INTR_MASK | 0x03D7E64C | ~BIT23 = **0xFF7FFFFF** (only CM3_FAULT unmasked; MSGQ stays masked, so HFI replies are always polled) | :1125, :947-948 |
| R9 | enable the `hfi` IRQ (SPI 304) | | | :1126 |
| R10 | `a6xx_gmu_set_initial_freq()`: sends HFI GX_BW_PERF_VOTE for `current_perf_index`, which is the highest level after probe (§5) | | | :1028-1040, :1129 |

On failure of R5/R6 (`:1131-1138`): disable the `gmu` IRQ, run `a6xx_rpmh_stop()` (§1.2), put gx, put the GMU (CX). **Clocks are not disabled on this path.** That is a Linux quirk or bug.

Afterwards `a6xx_gmu_pm_resume()` (`a6xx_gpu.c:2040-2061`) calls `msm_devfreq_resume` and `a6xx_llc_activate()`. For LLCC on sm8250, `have_mmu500 = true` (adreno_smmu is `arm,mmu-500`), so the SCIDs go into GPU register `GBIF_SCACHE_CNTL1`, not CX_MISC (`a6xx_gpu.c:1802-1860, 1914-1922`). That is outside the GMU scope.

### 1.4 `a6xx_gmu_fw_start()`: A650 cold boot (`a6xx_gmu.c:824-945`)

| # | Action | Address | Value / condition | Source |
|---|---|---|---|---|
| F1 | W GPU_GMU_CX_GMU_CX_FALNEXT_INTF ("vote veto for FAL10") | 0x03D7E3C4 | 0x1 | :834-836 |
| F2 | W GPU_GMU_CX_GMU_CX_FAL_INTF | 0x03D7E3C0 | 0x1 | :837 |
| F3 | W GMU_GENERAL_7 (TCM retention on) | 0x03D7E730 | 0x1 | :843 |
| F4 | `a6xx_rpmh_start()`: | | | :854, :516-541 |
| F4a | W GMU_RSCC_CONTROL_REQ | 0x03D8EC1C | 0x2 (BIT 1) | :521 |
| F4b | poll GMU_RSCC_CONTROL_ACK | 0x03D8EC20 | `val & BIT(1)`, 100 µs / 10 ms. On fail: "Unable to power on the GPU RSC", abort | :523-528 |
| F4c | poll RSCC_SEQ_BUSY_DRV0 | 0x03DE0404 | `val == 0`, 100 µs / 10 ms. On fail: "GPU RSC sequence stuck", abort | :530-536 |
| F4d | W GMU_RSCC_CONTROL_REQ | 0x03D8EC1C | 0x0 | :538 |
| F5 | `a6xx_gmu_fw_load()` (§1.5) | | | :858 |
| F6 | W GMU_CM3_FW_INIT_RESULT | 0x03D7E070 | 0x0 | :864 |
| F7 | W GMU_CM3_BOOT_CONFIG | 0x03D7E004 | 0x2 | :865 |
| F8 | W GMU_HFI_QTBL_ADDR | 0x03D7E614 | `hfi.iova` (32-bit GMU VA of the HFI BO) | :868 |
| F9 | W GMU_HFI_QTBL_INFO | 0x03D7E610 | 0x1 | :869 |
| F10 | W GMU_AHB_FENCE_RANGE_0 | 0x03D8EC44 | BIT31 \| (0xA << 18) \| 0xA0 = **0x802800A0** | :871-882 |
| F11 | W GMU_CM3_CFG | 0x03D7E0B4 | 0x4052 (avoids jumping to the NMI handler) | :888 |
| F12 | W GMU_HFI_SFR_ADDR (this register carries the chip id) | 0x03D7E608 | **0x06050200** (§1.6) | :890-911 |
| F13 | W GPU_GMU_CX_GMU_PWR_COL_CP_MSG | 0x03D7E400 | `log.iova \| (16K/4K − 1)` = `log.iova \| 3` (log iova is 4 KiB aligned) | :913-914 |
| F14 | `a6xx_gmu_power_config()`: | | | :918, :702-742 |
| F14a | W GMU_SYS_BUS_CONFIG | 0x03D7D03C | 0x1 | :708 |
| F14b | W GMU_ICACHE_CONFIG | 0x03D7D000 | 0x1 | :709 |
| F14c | W GMU_DCACHE_CONFIG | 0x03D7D004 | 0x1 | :710 |
| F14d | W GMU_PWR_COL_INTER_FRAME_CTRL | 0x03D7E300 | 0x09C40400 | :716 |
| F14e | (idle_level ACTIVE: no HYST/IFPC/SPTP writes) | | | :718-732 |
| F14f | RMW GMU_RPMH_CTRL \|= 0xF11 (bits 0,4,8,9,10,11) | 0x03D7E3A0 | read, OR 0x00000F11, write | :735-741 |
| F15 | `a6xx_gmu_start()`: | | | :920, :240-276 |
| F15a | R GMU_CM3_DTCM_START+0xFF8 (= GMU_CORE_FW_VERSION) | 0x03D74FE0 | if `val <= 0x20010004`: mask = 0xFFFFFFFF, expect = 0xBABEFACE. Else mask = 0x1FF, expect = 0x100. **linux-firmware a650_gmu.bin gives 0x20010008, so mask 0x1FF / expect 0x100** | :247-254 |
| F15b | W GMU_CM3_SYSRESET | 0x03D7E000 | 0x1 | :256 |
| F15c | W GPU_GMU_CX_GMU_PWR_COL_CP_RESP (log wptr index) | 0x03D7E404 | 0x0 | :264 |
| F15d | W GMU_CM3_SYSRESET | 0x03D7E000 | 0x0 (CM3 starts executing) | :267 |
| F15e | poll GMU_CM3_FW_INIT_RESULT | 0x03D7E070 | `(val & 0x1FF) == 0x100`, 100 µs / 10 ms. On fail: "GMU firmware initialization timed out" | :269-273 |
| F16 | (legacy `a6xx_gmu_gfx_rail_on`: **skipped** for A650) | | | :924-928 |
| F17 | `a6xx_sptprac_enable()`: returns 0 immediately because not legacy | | | :931-935, :406-407 |
| F18 | `a6xx_gmu_hfi_start()`: | | | :937, :278-291 |
| F18a | W GMU_HFI_CTRL_INIT | 0x03D7E618 | 0x1 | :283 |
| F18b | poll GMU_HFI_CTRL_STATUS | 0x03D7E600 | `val & 1`, 100 µs / 10 ms. On fail: "Unable to start the HFI queues" | :285-288 |
| F19 | `wmb()` | | | :942 |

Note on F15: Linux does not assert `CM3_SYSRESET` before writing ITCM/DTCM in F5. It relies on the CM3 being in reset after a CX power collapse. **XNU suggestion: write `GMU_CM3_SYSRESET = 1` before F5** if CX might not have been collapsed, for example after a failed boot. `a6xx_gmu_force_off` uses the same register to halt the core (`a6xx_gmu.c:1020`).

### 1.5 `a6xx_gmu_fw_load()` and the `a650_gmu.bin` format (`a6xx_gmu.c:744-822`)

The file is a flat concatenation of blocks, all little-endian:

```
struct block_header {        // a6xx_gmu.c:744-750
    u32 addr;    // GMU-virtual destination address
    u32 size;    // payload size in BYTES (multiple of 4)
    u32 type;    // ignored by Linux
    u32 value;   // ignored by Linux
    u32 data[];  // 'size' bytes of payload
};
next = (u8*)blk + 16 + size      // a6xx_gmu.c:792  (&blk->data[size>>2])
```

Loop until `blk >= fw + fw_size` (`:790-792`):
* `size == 0`: skip (`:793-794`). These are descriptor entries.
* `addr` in `[0x00000000, 0x00004000)` (ITCM, `in_range` checks only the start address): bulk-write `size` bytes with `memcpy_toio` to `GMU_CM3_ITCM_START + (addr − 0)/4`. That is physical `0x03D6D000 + addr`. Then `wmb` (`:796-800`, `a6xx_gmu.h:132-137`).
* else if `addr` in `[0x10004000, 0x10008000)` (DTCM; A650 `dtcm_base = 0x10004000`, `:773-774`): write to `GMU_CM3_DTCM_START + (addr − 0x10004000)/4`. That is physical `0x03D71000 + (addr − 0x10004000)` (`:801-805`).
* else: `memcpy` into whichever BO contains `addr` (`[bo.iova, bo.iova + bo.size)`), checked in the order icache, dcache, dummy. Destination is `bo.virt + addr − bo.iova` (`fw_block_mem`, `:752-759, 806-808`). If none matches, Linux logs "failed to match fw block" and **continues** (not fatal).
* No other checks are made. Linux does not check that the block end stays inside the TCM or BO, does not check a checksum, and does not use type or value. (The legacy whole-image ITCM copy with its 0x8000 size check at `:776-787` does not apply.)
* Afterwards it reads `GMU_CORE_FW_VERSION` (0x03D74FE0) and prints major [31:28], minor [27:16], step [15:0] (`:815-819`).

Contents of the linux-firmware `a650_gmu.bin` actually parsed for this spec (41548 bytes, sha256 `df79fa84…b01c`, obtained from a linux-firmware mirror; **check this against the blob on the target device**):

| file off | addr | size | type | value | goes to |
|---|---|---|---|---|---|
| 0x00000 | 0x00000000 | 0 | 0x1 | 0x200 | descriptor (skipped) |
| 0x00010 | 0x00000200 | 0 | 0x1 | 0x200 | descriptor |
| 0x00020 | 0x00000400 | 0 | 0x1 | 0x3C00 | descriptor |
| 0x00030 | 0x00004000 | 0 | 0x1 | 0x8000 | descriptor |
| 0x00040 | 0x10004000 | 0 | 0x1 | 0x3FE0 | descriptor |
| 0x00050 | 0x10007FDC | 0 | 0x1 | 0x24 | descriptor |
| 0x00060 | 0x00144000 | 0 | 0x1 | 0x13000 | descriptor (region inside icache BO) |
| 0x00070 | 0x00157000 | 0 | 0x1 | 0x9000 | descriptor (region inside icache BO) |
| 0x00080 | 0x60000000 | 0 | **0x7** | 0x1000 | "prealloc" request for 4 KiB @ 0x60000000, satisfied by the dummy BO |
| 0x00090 | 0x00000000 | 0xC0 | 0xDEADBEEF | 0xDEADBEEF | ITCM (vector table: SP = 0x10007F80, reset = 0x11) |
| 0x00160 | 0x00000200 | 0x140 | 0xDEADBEEF | 0xDEADBEEF | ITCM |
| 0x002B0 | 0x00000400 | 0x370C | 0xDEADBEEF | 0xDEADBEEF | ITCM |
| 0x039CC | 0x00004000 | 0x55D0 | 0xDEADBEEF | 0xDEADBEEF | icache BO (offset 0) |
| 0x08FAC | 0x00157000 | 0x398 | 0xDEADBEEF | 0xDEADBEEF | icache BO (offset 0x153000) |
| 0x09354 | 0x10004000 | 0xEC0 | 0xDEADBEEF | 0xDEADBEEF | DTCM |
| 0x0A224 | 0x10007FDC | 0x18 | 0xDEADBEEF | 0xDEADBEEF | DTCM tail: 0xFFFFFFFF, **0x20010008** (@0x10007FE0 = FW version, v2.1.8), 0xFFFFFFFF, 0x20010008, 0, 0x20000001 |

The firmware touches GMU VAs up to 0x160000 in the icache region and 4 KiB at 0x60000000. It never uses a dcache BO (none is allocated on A650).

### 1.6 Chip id written to `GMU_HFI_SFR_ADDR` (`a6xx_gmu.c:890-911`)

`chipid = (0x06050002 & 0xFFFF0000) | ((0x06050002 << 4) & 0xF000) | ((0x06050002 << 8) & 0x0F00)`
= 0x06050000 | 0x0000 | 0x0200 = **0x06050200**. The upper 16 bits are core/major. Bits 15:12 are minor (0). Bits 11:8 are patch (2). Bits 7:0 are 0. KGSL also uses this register to pass the chip id.

### 1.7 State after §1.3

The GMU runs firmware v2.1.8. HFI queues are live. The perf and BW tables, CORE_FW_START and START have been acked, and a GX perf vote has been acked. **GX is not guaranteed to be powered until the host does the OOB GPU_SET handshake** (§4). That is the first thing `hw_init` does.

### 1.8 Hand-off into GPU `hw_init` (`a6xx_gpu.c:1064-1078, 1401-1420`)

1. Take the GMU lock. `a6xx_gmu_set_oob(GMU_OOB_GPU_SET)`: W GMU_HOST2GMU_INTR_SET = `1<<30`, poll GMU2HOST_INTR_INFO bit 31 (100 µs / 10 ms), then W GMU2HOST_INTR_CLR = `1<<31` (§4). If it fails, hw_init aborts.
2. After that, GPU registers are safe to touch. Clear the GBIF halt: W GBIF_HALT (0x03D0F114) = 0 and read it back, then W RBBM_GBIF_HALT (0x03D00058) = 0 and read it back (`a6xx_gpu.c:1087-1093`). Then the rest of hw_init (separate spec).
3. At the end: `a6xx_gmu_clear_oob(GPU_SET)`: W GMU_HOST2GMU_INTR_SET = `1<<31` (`:1412`). There is no BOOT_SLUMBER clear because A650 is not legacy (`:1414-1417`).

### 1.9 GMU-space writes made by GPU `hw_init` (for completeness)

* `a6xx_set_hwcg(gpu, true)` (`a6xx_gpu.c:511-580`, called at `:1125`):
  * W GPU_GMU_AO_GMU_CGC_MODE_CNTL (0x03D8EC24) = **0x00020202**
  * W GPU_GMU_AO_GMU_CGC_DELAY_CNTL (0x03D8EC28) = **0x00010111**
  * W GPU_GMU_AO_GMU_CGC_HYST_CNTL (0x03D8EC2C) = **0x00005555**
  * If `RBBM_CLOCK_CNTL != 0x8AA8AA82`: RMW GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL (0x03D6A200) to clear bit 0, write the `a650_hwcg` list, RMW to set bit 0, then W RBBM_CLOCK_CNTL = 0x8AA8AA82.
* W GPU_GMU_AO_GPU_CX_BUSY_MASK (0x03D8EC38) = 0xFF000000 (`:1256`).
* RMW GMU_CX_GMU_POWER_COUNTER_SELECT_0 (0x03D7E104): clear 0xFF, set 0x20 (`:1259`).
* W GMU_CX_GMU_POWER_COUNTER_ENABLE (0x03D7E100) = 1 (`:1260`).
* The busy counter is read from XOCLK_0_L/H (0x03D7E110/0x03D7E114) (`a6xx_gpu.c:2234-2250`).

---------------------------------------------------------------------------------------------------

## 2. GMU memory and SMMU mapping

### 2.1 How Linux maps it

* `msm_iommu_new(gmu->dev, 0)` (`a6xx_gmu.c:1320`, `msm_iommu.c:717-751`) calls `iommu_paging_domain_alloc(gmu_dev)` and `iommu_attach_device`. This is a **plain stage-1 paging domain** on the arm-smmu (qcom adreno-smmu impl) context bank assigned to the GMU stream (SID 5, mask 0x400). No page-table quirks (`quirks = 0`). No split TTBR (that is only for the GPU's own SID-0 domain). Linux uses the AArch64 LPAE format with a 4 KiB granule (`PAGE_SIZE` = 4K; pgsize bitmap 4K/2M/1G).
* VM: `msm_gem_vm_create(drm, mmu, "gmu", 0x0, 0x80000000, managed = true)` (`:1326`). GMU VAs are 32-bit and live in `[0, 0x80000000)`.
* Each BO is `msm_gem_new(size, MSM_BO_WC [| MSM_BO_MAP_PRIV])` backed by shmem pages. **The physical pages are scattered**, with no contiguity requirement. They are pinned and mapped with `iommu_map_sgtable` (`msm_iommu.c:670-687`). The CPU mapping is write-combined (uncached).
* IOMMU prot comes from mainline `msm_gem_prot()` (that file is not in this tree; from memory): `IOMMU_READ | IOMMU_WRITE`, plus `IOMMU_PRIV` when `MSM_BO_MAP_PRIV`. **No `IOMMU_CACHE`** (non-cacheable for the device). **No `IOMMU_NOEXEC`** (executable).
* `MSM_BO_MAP_PRIV` is added for every fixed-address BO (dummy, icache) (`a6xx_gmu.c:1290-1295`). In io-pgtable-arm, PRIV means "AP: no EL0 access". Probable reason (inference): in the ARMv8 stage-1 format, a page writable by unprivileged accesses is implicitly privileged-execute-never. The CM3 fetches code from the icache region as privileged, so that region must be mapped privileged-only.
* Floating BOs (log, hfi) go through `drm_mm` in `[0x60000000 + 4K, 0x80000000)`, the "GMU uncached range" (`:1285-1289`). With Linux's allocation order (dummy, icache, log, hfi) and best-fit `drm_mm`, **log usually lands at 0x60001000 and hfi at 0x60005000**. The firmware learns both addresses from registers (F8, F13), so any 4 KiB-aligned address in `[0x60001000, 0x80000000)` should work.
* Sizes are `PAGE_ALIGN`ed (`:1284`).

### 2.2 A650 regions

| Name | GMU VA | Size | Linux flags (SMMU prot) | Holds | Source |
|---|---|---|---|---|---|
| dummy | **0x60000000** (fixed) | 0x1000 (4 KiB; 8 KiB only on a660/a7xx) | WC + MAP_PRIV (RW, priv-only, exec, non-cacheable) | Nothing from the host. It satisfies the firmware's type-7 "prealloc" block at 0x60000000/0x1000 | `a6xx_gmu.c:1956, 1968-1971` |
| icache | **0x00004000** (fixed) | 0x00FFC000 (16 MiB − 16 KiB), so it ends at 0x01000000 | WC + MAP_PRIV | Firmware code/data blocks at 0x4000 (0x55D0 B) and 0x157000 (0x398 B). Firmware descriptors also reference 0x144000..0x160000 | `a6xx_gmu.c:1974-1979` |
| dcache | — | not allocated on A650 (only a640 family) | — | — | `a6xx_gmu.c:1986-1995` |
| debug | — | not allocated on A650 (a660/a7xx: 28 KiB @ 0x60400000; a630: 16 KiB) | — | — | `a6xx_gmu.c:1957-1965, 2000-2003` |
| log | floating, ≥ 0x60001000 (typically 0x60001000) | 0x4000 | WC (RW, priv+unpriv, exec, non-cacheable) | GMU firmware log ring. Address and size are passed via `PWR_COL_CP_MSG = iova \| 3`; the wptr index is reset via `PWR_COL_CP_RESP = 0` | `a6xx_gmu.c:2007, 913-914, 264` |
| hfi | floating (typically 0x60005000) | 0x4000 | WC | HFI queue table + command queue + response queue (§3.1) | `a6xx_gmu.c:2012, 868` |

Constraints for XNU:
* The SID-5 context bank must translate those VAs with RW permission. Code regions should be privileged and not XN. Use the non-cacheable (or WC) memory type unless coherency is handled; Linux uses non-cacheable on the device side and WC on the CPU side.
* The firmware probably needs only up to 0x160000 of the icache region. Linux maps the whole 16 MiB − 16 KiB. **Recommendation: allocate it all** (scattered 4 KiB pages are fine). Shrinking it is an untested deviation.
* The ITCM (GMU VA 0..0x3FFF) and DTCM (0x10004000..0x10007FFF) are **not** SMMU-mapped. The host fills them through the MMIO windows.
* CPU writes into icache must be visible (flush WC) before F15d releases the CM3.

---------------------------------------------------------------------------------------------------

## 3. HFI protocol (A650 = HFI v2 / non-legacy)

### 3.1 Queue table in the hfi BO (`a6xx_hfi.c:937-997`, `a6xx_hfi.h:7-29`)

Written once at probe by `a6xx_hfi_init`, and **not rewritten on resume**. Only the read/write indices are reset, by `a6xx_hfi_stop` (§6).

| hfi BO offset | Field | Value |
|---|---|---|
| 0x000 | table.version | 0 |
| 0x004 | table.size | **120** (= 24 + 2×48, in **bytes**; the header comment says "dwords", and KGSL is believed to use dwords. The firmware evidently tolerates it) |
| 0x008 | table.qhdr0_offset | 6 (dwords) |
| 0x00C | table.qhdr_size | 12 (dwords) |
| 0x010 | table.num_queues | 2 |
| 0x014 | table.active_queues | 2 |
| 0x018 | qhdr[0] (command queue, host to GMU), 12 dwords: | |
| +0x00 | status | 1 |
| +0x04 | iova | hfi.iova + 0x1000 |
| +0x08 | type | `(10 << 8) \| 0` = 0x0A00 |
| +0x0C | size | 0x400 (dwords = 4 KiB) |
| +0x10 | msg_size | 0 |
| +0x14 | dropped | 0 |
| +0x18 | rx_watermark | 1 |
| +0x1C | tx_watermark | 1 |
| +0x20 | rx_request | 1 |
| +0x24 | tx_request | 0 |
| +0x28 | read_index | 0 (dwords; advanced by the GMU) |
| +0x2C | write_index | 0 (dwords; advanced by the host) |
| 0x048 | qhdr[1] (response queue, GMU to host) | same as qhdr[0] except iova = hfi.iova + 0x2000 and type = `(10 << 8) \| 1` = **0x0A01** (legacy would use id 4) |
| 0x1000..0x1FFF | command queue ring | |
| 0x2000..0x2FFF | response queue ring | |
| 0x3000..0x3FFF | unused | |

### 3.2 Message header and sending (`a6xx_hfi.c:189-210, 67-101`; `a6xx_hfi.h:55-66`)

```
header = (seqnum << 20) | (HFI_MSG_CMD(0) << 16) | (size_in_dwords << 8) | msg_id
         [31:20] seq    [19:16] type        [15:8] size (includes header)  [7:0] id
seqnum = (++cmdq.seqnum) % 0xFFF     // starts at 0 at probe, so the first message uses 1. Not reset on resume.
```

Write procedure (`a6xx_hfi_queue_write`):
1. `space = CIRC_SPACE(wr, rd, 1024)`. If `space < dwords`, increment `dropped` and return -ENOSPC.
2. Copy the dwords to `ring[wr..]`, wrapping modulo 1024.
3. **Non-legacy padding**: fill with 0xFAFAFAFA until the index is a multiple of 4.
4. Set `write_index` to the new index.
5. W GMU_HOST2GMU_INTR_SET (0x03D7E650) = 0x1.

Wait for the ack (`a6xx_hfi_wait_for_ack`, `:126-187`):
1. poll GMU_GMU2HOST_INTR_INFO (0x03D7E648) for bit 0 (MSGQ). Interval 100 µs, **timeout 1 s** (`:109-110`). On timeout: -ETIMEDOUT.
2. W GMU_GMU2HOST_INTR_CLR (0x03D7E644) = 0x1.
3. Read one message from the response queue (`a6xx_hfi_queue_read`, `:29-65`):
   * If `rd == wr`, set `rx_request = 1` and treat it as empty: go back to step 1 (one more 1 s wait). If it is still empty, fail.
   * Otherwise `hdr = ring[rd]`, `n = (hdr >> 8) & 0xFF`. Linux BUG()s if `n > 19`. Copy `n` dwords, wrapping. Then `rd = ALIGN(rd, 4) % 1024` (non-legacy) and write it to `read_index`.
4. Response layout `a6xx_hfi_msg_response { u32 header; u32 ret_header; u32 error; u32 payload[16]; }`. The ack id is `HFI_F2H_MSG_ACK = 126`; Linux does not check it. `ret_header` echoes the request header.
   * If `(header & 0xFF) == 100` (`HFI_F2H_MSG_ERROR {header, code, payload[2]}`): log `code` and read the next message.
   * If `(ret_header >> 20) & 0xFFF != seqnum`: log and read the next message.
   * If `error != 0`: fail with -EINVAL.
   * Otherwise success, and copy out up to 16 payload dwords.

There are no host-side interrupts for HFI replies; everything is polled. `MSGQ` stays masked in GMU2HOST_INTR_MASK.

### 3.3 Message catalogue relevant to A650 (`a6xx_hfi.h`)

| ID | Name | Dwords | Header low 16 bits | Layout (dword index: field) |
|---|---|---|---|---|
| 4 | H2F_MSG_PERF_TABLE (v2) | 59 (0x3B) | 0x3B04 | 0: hdr; 1: num_gpu_levels; 2: num_gmu_levels; 3+3i: gx[i].vote, 4+3i: gx[i].acd, 5+3i: gx[i].freq (i = 0..15); 51+2j: cx[j].vote, 52+2j: cx[j].freq (j = 0..3) |
| 3 | H2F_MSG_BW_TABLE | 160 (0xA0) | 0xA003 | 0: hdr; 1: bw_level_num; 2: cnoc_cmds_num; 3: ddr_cmds_num; 4: cnoc_wait_bitmask; 5: ddr_wait_bitmask; 6..11: cnoc_cmds_addrs[6]; 12..23: cnoc_cmds_data[2][6] (row-major); 24..31: ddr_cmds_addrs[8]; 32..159: ddr_cmds_data[16][8] (row-major, [i][j] at 32 + 8i + j) |
| 11 | H2F_FEATURE_CTRL | 4 | 0x040B | hdr, feature, enable, data (ACD only; not sent on mainline A650) |
| 7 | H2F_MSG_ACD | 37 (0x25) | 0x2507 | hdr, version, enable_by_level, stride, num_levels, data[32] (not sent) |
| 14 | H2F_MSG_CORE_FW_START | 2 | 0x020E | hdr, handle = 0 |
| 10 | H2F_MSG_START | 1 | 0x010A | hdr |
| 30 | H2F_MSG_GX_BW_PERF_VOTE | 4 | 0x041E | hdr, ack_type, freq (perf index), bw (bw index / AB vote) |
| 33 | H2F_MSG_PREPARE_SLUMBER | 3 | 0x0321 | hdr, bw = 0, freq = 0 |
| 0/1/5 | INIT / FW_VERSION / TEST | — | — | **legacy (HFI v1) only. Not sent on A650** (`a6xx_hfi.c:846-879`) |
| 126 | F2H_MSG_ACK | ≤ 19 | — | header, ret_header, error, payload[16] |
| 100 | F2H_MSG_ERROR | 4 | — | header, code, payload[2] |

### 3.4 Messages sent during start, in order (`a6xx_hfi_start`, `a6xx_hfi.c:881-914`)

Every message is synchronous: send, then wait for the ack. Any failure aborts the resume.

1. **PERF_TABLE** (`a6xx_hfi_send_perf_table`, `:257-278`):
   * `num_gpu_levels = nr_gpu_freqs` (OPP count + 1), `num_gmu_levels = nr_gmu_freqs` (= 2).
   * `gx_votes[i] = { vote = gx_arc_votes[i], acd = 0xFFFFFFFF, freq = gpu_freqs[i] / 1000 (kHz) }` for i in [0, nr_gpu_freqs). Index 0 is the "off" level (freq 0).
   * `cx_votes[j] = { vote = cx_arc_votes[j], freq = gmu_freqs[j] / 1000 }`, so `{V_cx(0), 0}` and `{V_cx(48), 200000}`.
   * Unused entries are 0.
   * Example for a 7-OPP speed bin: freqs (kHz) = [0, 305000, 400000, 441600, 490000, 525000, 587000, 670000]; num_gpu_levels = 8. The vote values come from cmd-db (§3.5).
2. **BW_TABLE**, A650 static table (`a650_build_bw_table`, `a6xx_hfi.c:448-477`; chosen at `:744-745` because `bcms == NULL`). Built once and cached in `gmu->bw_table`. Everything not listed is 0:
   ```
   bw_level_num       = 1
   ddr_cmds_num       = 3
   ddr_wait_bitmask   = 0x01
   ddr_cmds_addrs[0]  = 0x50000
   ddr_cmds_addrs[1]  = 0x50004
   ddr_cmds_addrs[2]  = 0x5007c
   ddr_cmds_data[0][0] = 0x40000000
   ddr_cmds_data[0][1] = 0x40000000
   ddr_cmds_data[0][2] = 0x40000000
   cnoc_cmds_num      = 1
   cnoc_wait_bitmask  = 0x01
   cnoc_cmds_addrs[0] = 0x500a4
   cnoc_cmds_data[0][0] = 0x40000000
   cnoc_cmds_data[1][0] = 0x60000001
   ```
   BCM TCS encoding (mainline `include/soc/qcom/tcs.h`, not in tree): `BCM_TCS_CMD(commit, valid, x, y) = commit<<30 | valid<<29 | (x & 0x3FFF)<<14 | (y & 0x3FFF)`.
   * 0x40000000 = commit, not valid: a "zero / off" vote.
   * 0x60000001 = commit, valid, y = 1: the CNOC "on" vote.
   * The addresses are RPMh BCM resource addresses (slave id 5, so 0x5xxxx). Mainline does not say which names they are. See §7.
   * **Effect: the GMU has only one DDR level ("off"), so the GMU firmware never raises DDR bandwidth.**
3. **ACD**: skipped (`a6xx_hfi_enable_acd` returns 0 when `enable_by_level == 0`, `:780-781`).
4. **CORE_FW_START** `{hdr, handle = 0}` (`:816-822, 901`).
5. **START** `{hdr}` (`:808-814, 909`). The Linux comment notes that downstream sends it in hw_init.

Then (§1.3 R10) **GX_BW_PERF_VOTE** with the initial level (§5).

Sequence numbers on the very first boot after probe: PERF_TABLE = 1, BW_TABLE = 2, CORE_FW_START = 3, START = 4, GX_BW_PERF_VOTE = 5. Later boots continue counting.

### 3.5 Frequency tables and RPMh ARC votes (`a6xx_gmu.c:1431-1666`)

**Tables** (`a6xx_gmu_build_freq_table`, `:1561-1591`): `freqs[0] = 0` ("off"), then OPPs in **ascending** order. Max sizes: GX 16, CX 4.
* GMU (CX) table: [0, 200000000], so `nr_gmu_freqs = 2`.
* GPU table: depends on the speed bin (`a6xx_gpu.c:2337-2382`).
  * The fuse is read from qfprom `0x00784000 + 0x19B`, bits [7:5] (`sm8250.dtsi:972-981`).
  * Mapping {fuse: index}: {0: 0, 1: 1, 2: 3, 3: 2} (`a6xx_catalog.c:983-988`). The OPP is enabled when `opp-supported-hw & BIT(index)`. An unknown fuse falls back to BIT(0).

| fuse | supp_hw | OPPs (MHz, ascending) | nr_gpu_freqs |
|---|---|---|---|
| 0 | 0x1 | 305, 400, 441.6, 490, 525, 587 | 7 |
| 1 | 0x2 | 305 … 587, 670 | 8 |
| 2 | 0x8 | 305 … 587, 670 | 8 |
| 3 | 0x4 | 305, 400, 441.6, 490, 525 | 6 |

Stock DT (`kona-v21.dts:23853-24333`) has bins 0/1/2/4 = 7 levels with 670 MHz and bin 3 = 5 levels (525 MHz max). **Mainline drops 670 MHz for fuse 0, but stock keeps it.**

OPP → RPMh level (`opp-level` in `sm8250.dtsi:2986-3030`; the same values appear as `opp-microvolt` in stock `kona-v21.dts:24440-24478`):

| MHz | 305 | 400 | 441.6 | 490 | 525 | 587 | 670 | GMU 200 |
|---|---|---|---|---|---|---|---|---|
| level | 64 LOW_SVS | 128 SVS | 144 SVS_L0 | 192 SVS_L1 | 224 SVS_L2 | 256 NOM | 320 NOM_L1 | 48 MIN_SVS |

Freq 0 maps to level 0 (`a6xx_gmu_get_arc_level` returns 0 for freq 0 or an unknown freq, `:1432-1450`).

**Vote construction** (`a6xx_gmu_rpmh_arc_votes_init`, `:1452-1528`):
* GX: primary = cmd-db aux data of `"gfx.lvl"`, levels from the **GPU** device OPPs (`:1547-1548`).
* CX: primary = `"cx.lvl"`, levels from the **GMU** device OPP (`:1551-1552`).
* Secondary for both: `"gmxc.lvl"` if present (expected absent on SM8250), else `"mx.lvl"` (`:1474-1478`).
* Aux data is an array of little-endian `u16` RPMh levels (vlvl), indexed by hlvl. Count = aux_len / 2. An empty array is an error.

For each frequency with level L:
```
pindex = first j with pri[j] >= L          (error "Level not found" if none)
sindex = first j with sec[j] >= L; if none, the last j with sec[j] != 0
vote   = (pri[pindex] & 0xFFFF) << 16 | (sindex & 0xFF) << 8 | (pindex & 0xFF)
```
So bits 31:16 are the actual primary vlvl, bits 15:8 the secondary (MX) hlvl index, and bits 7:0 the primary hlvl index.

Hypothetical example (a typical table; **read the real one from cmd-db**): gfx.lvl = mx.lvl = [0, 16, 48, 64, 128, 192, 256, 320, 384, 416].
* 670 MHz (L = 320): pindex 7, sindex 7, vote = 0x01400707.
* off (L = 0): vote = (pri[0] << 16) | 0 | 0.

The legacy-only `gfx_rail_on` would use `vote & 0xFF` and `(vote >> 8) & 0xFF` (`:453-456`); A650 does not.

BW/BCM votes (`a6xx_gmu_rpmh_bw_votes_init`, `:1347-1429`) are **not used on A650** (`bcms == NULL`).

---------------------------------------------------------------------------------------------------

## 4. OOB handshakes (`a6xx_gmu.c:293-398`)

Mechanism:
* set: W GMU_HOST2GMU_INTR_SET (0x03D7E650) = `1 << req`. Then poll GMU_GMU2HOST_INTR_INFO (0x03D7E648) for `1 << ack`, 100 µs / 10 ms. Then W GMU_GMU2HOST_INTR_CLR (0x03D7E644) = `1 << ack`. The CLR write happens even on timeout.
* clear: W GMU_HOST2GMU_INTR_SET = `1 << clr`. There is no wait.
* Callers hold the GMU mutex.

A650 uses the **new** bits (`legacy == false`):

| OOB | req (set_new) | ack (ack_new) | clear (clear_new) | A650 use |
|---|---|---|---|---|
| GPU_SET | **30** | **31** | **31** | hw_init start/end (§1.8). Also a dummy set+clear in shutdown when `needs_hw_init` (`:1171-1176`). Keeps GX powered while the CPU touches GPU registers |
| PERFCOUNTER_SET | 28 | 30 | 29 | around `CP_ALWAYS_ON_COUNTER` reads (`a6xx_gpu.c:2177-2194`) |
| BOOT_SLUMBER | — (0) | — | — | legacy only (old bits: set 22 / ack 30 / clear 30). On A650 `set_oob` returns -EINVAL ("Invalid non-legacy GMU request") |
| DCVS_SET | — (0) | — | — | legacy only (old bits: set 23 / ack 31 / clear 31). A650 uses HFI instead |

Old-style GPU_SET bits are set 16 / ack 24 / clear 24 (`:302-310`). The `a6xx_gmu.xml` `A6XX_GMU_OOB` bitset (GPU 18/26 etc.) is **not** what Linux uses. Ignore it.

Note: on new firmware the GPU_SET ack bit (31) and clear bit (31) are the same, and the host clears the ack in GMU2HOST_INTR_CLR right away.

---------------------------------------------------------------------------------------------------

## 5. Frequency change (`a6xx_gmu_set_freq`, `a6xx_gmu.c:109-203`; `a6xx_hfi_set_freq`, `a6xx_hfi.c:824-834`)

1. If `target == gmu->freq`, do nothing.
2. `perf_index` = the index in `gpu_freqs[]` whose value equals the OPP frequency. If none matches, it ends up as `nr_gpu_freqs − 1`.
3. `bw_index = 0` (A650 has no `bcms`; the AB vote encoding at `:139-158` is a750-only).
4. Update `current_perf_index` and `gmu->freq`. If the GPU is suspended, stop here; the new level is applied on the next resume via `set_initial_freq`.
5. Non-legacy: send **GX_BW_PERF_VOTE** `{ header = (seq<<20) | 0x041E, ack_type = 1 (blocking), freq = perf_index, bw = 0 }` and wait for the ack (§3.2).
6. Because `bw_index == 0`, call `dev_pm_opp_set_opp(gpu_dev, opp)`, which is a no-op on SM8250.

Example (8-level bin): 670 MHz → freq = 7, 587 → 6, 525 → 5, 490 → 4, 441.6 → 3, 400 → 2, 305 → 1. Index 0 is "off" and is never requested.

After probe, `current_perf_index = nr_gpu_freqs − 1` (`:1654`), so the first boot votes the **highest** level. Stock KGSL uses `initial-pwrlevel = 6` (lowest, 305 MHz).

The legacy DCVS register path (`:183-202`) is not used on A650.

---------------------------------------------------------------------------------------------------

## 6. Power down (`a6xx_gmu_stop`, `a6xx_gmu.c:1225-1258`)

Graceful path (`a6xx_gmu_shutdown`, `:1159-1222`):
1. If `needs_hw_init` (GPU was resumed but hw_init never ran): OOB GPU_SET set, then clear. On failure, force off.
2. `a6xx_gmu_wait_for_idle`: spin up to 1 s until GPU_GMU_CX_GMU_RPMH_POWER_STATE (0x03D7E3B0) == idle_level (0 = ACTIVE) (`:214-238`, `adreno_gpu.h:242-254`). On failure, force off.
3. `a6xx_bus_clear_pending_transactions(gx_off = a6xx_gpu->hung)` (`a6xx_gpu.c:1987-2022`):
   * If hung: W RBBM_GBIF_HALT = 1 and spin until RBBM_GBIF_HALT_ACK & 1.
   * Always: W GBIF_HALT = 1, spin until (ACK & 1); W GBIF_HALT = 2, spin until (ACK & 2); W GBIF_HALT = 0. The spins time out after 1 s.
4. `a6xx_gmu_notify_slumber` (`:477-514`):
   * W GMU_CX_GMU_POWER_COUNTER_ENABLE = 0.
   * `sptprac_disable` is a no-op.
   * HFI **PREPARE_SLUMBER** `{hdr | 0x0321, bw = 0, freq = 0}`.
   * The gemnoc workaround is not for A650.
   * W GMU_AO_AHB_FENCE_CTRL (0x03D8EC40) = 0 (fence in allow mode).
   * On failure, force off.
5. poll GPU_CX_BUSY_STATUS (0x03D8EC30) until bit 23 is clear, 100 µs / 10 ms. On failure it only logs.
6. `a6xx_hfi_stop`: set both queues' `read_index = write_index = 0` (`a6xx_hfi.c:916-935`).
7. `a6xx_gmu_irq_disable`: disable both IRQs, W GMU_AO_HOST_INTERRUPT_MASK = 0xFFFFFFFF, W GMU_GMU2HOST_INTR_MASK = 0xFFFFFFFF (`:955-962`).
8. `a6xx_rpmh_stop` (§1.2).

Force-off path (`a6xx_gmu_force_off`, `:986-1026`; also used when `gmu->hung`):
* W GMU_GMU_PWR_COL_KEEPALIVE = 0.
* hfi_stop and irq_disable.
* `a6xx_gmu_rpmh_off`: poll RSCC TCS0/1/2 DRV0_STATUS (0x03DE0D18/0FB8/1258) for bit 0, 100 µs / 10 ms each, and TCS3 (0x03DE14F8) with a 1 ms timeout.
* W GMU_AHB_FENCE_STATUS_CLR = 0x7, W GMU_AO_AHB_FENCE_CTRL = 0, `wmb`.
* W GMU_CM3_SYSRESET = 1 (halt the CM3).
* `bus_clear_pending_transactions(gx_off = true)`.
* `a6xx_gpu_sw_reset(true)`: W RBBM_SW_RESET_CMD = 1, read it back, wait 1 µs + 100 µs. **The reset is left asserted.** It is deasserted on the next hw_init path.

Then in `a6xx_gmu_stop`:
* `dev_pm_opp_set_opp(NULL)` (no-op on SM8250).
* `pm_runtime_put_sync(gxpd)`: this really disables the GX GDSC if the GMU left it on.
* Disable all GMU clocks.
* `pm_runtime_put_sync(gmu)`: CX GDSC off.

Before this, `a6xx_gmu_pm_suspend` deactivates the LLCC slices (`a6xx_gpu.c:2114-2139`).

IRQ handlers while running:
* `gmu` IRQ: read and clear AO_HOST_INTERRUPT_STATUS. WDOG_BITE marks the GMU hung and runs recovery. AHB bus error is logged. FENCE_ERR is logged with AHB_FENCE_STATUS (`a6xx_gmu.c:37-59`).
* `hfi` IRQ: read and clear GMU2HOST_INTR_INFO. CM3_FAULT marks the GMU hung and runs recovery (`:61-76`).

---------------------------------------------------------------------------------------------------

## 7. Open questions, cmd-db dependencies, and risks

### 7.1 cmd-db without Linux

* **Location:** reserved memory `0x80860000`, size `0x20000`, `compatible = "qcom,cmd-db"`, no-map (`sm8250.dtsi:782-786`; stock `kona-v21.dts:24533-24538`). Map it read-only and uncached. The header is at the base.
* **Format** (mainline `drivers/soc/qcom/cmd-db.c`; that file is not in this tree, so this is from memory and must be checked against the hardware dump):
  ```
  struct cmd_db_header {            // at 0x80860000
      le32 version;
      u8   magic[4];                // must be { 0xDB, 0x30, 0x03, 0x0C }
      struct rsc_hdr header[8];     // MAX_SLV_ID = 8, 16 bytes each
      le32 checksum;
      le32 reserved;
      u8   data[];                  // at offset 0x90
  };
  struct rsc_hdr {                  // 16 bytes
      le16 slv_id;                  // 3 = ARC, 4 = VRM, 5 = BCM; 0 ends the list
      le16 header_offset;           // entries at data + header_offset
      le16 data_offset;             // aux data base at data + data_offset
      le16 cnt;                     // number of entries
      le16 version;
      le16 reserved[3];
  };
  struct entry_header {             // 24 bytes
      u8   id[8];                   // NUL-padded resource name, e.g. "gfx.lvl\0"
      le32 priority[2];
      le32 addr;                    // RPMh resource address (cmd_db_read_addr)
      le16 len;                     // aux data length in bytes
      le16 offset;                  // aux data at data + rsc.data_offset + offset
  };
  ```
* **Lookup:** pad the query to 8 bytes with NULs. For `i` in 0..7, stop if `header[i].slv_id == 0`. Otherwise compare the 8-byte `id` of each of the `cnt` entries at `data + header_offset`.
  * `cmd_db_read_addr` returns `entry.addr`.
  * `cmd_db_read_aux_data` returns a pointer to `data + data_offset + entry.offset` with length `entry.len`.
* **What A650 bring-up needs:**
  1. `"gfx.lvl"` aux data (u16[]): primary for the GX votes.
  2. `"cx.lvl"` aux data: primary for the CX (GMU) votes.
  3. `"mx.lvl"` aux data: secondary for both. Try `"gmxc.lvl"` first and fall back to `"mx.lvl"` to mirror Linux.
  4. Optional, only for verifying the static BW table: the `addr` of the BCMs. KGSL on kona is believed to use "SH0", "MC0" and "ACV" for DDR and "CN0" for CNOC. **Open question:** check that these resolve to 0x50000 / 0x50004 / 0x5007C / 0x500A4, and in what order. Mainline hard-codes them with no names.
* Precomputing: the cmd-db content is fixed per SoC/firmware build. An XNU bring-up could dump it once (for example from Linux `/sys/kernel/debug/qcom_cmd_db`) and hard-code the vote words. Parsing at boot is more robust.

### 7.2 Open questions and risks

1. **No GPU DDR bandwidth vote** on mainline SM8250: no interconnects on the GPU node, and the BW table has a single "off" level. GPU memory throughput depends on other DDR voters. Stock KGSL has 12-level `qcom,gpu-bus-table` tables (`kona-v21.dts:23784-23798`) and votes DDR through the GMU. Expect poor performance until a real BW table is built (cmd-db BCM aux data plus the `a6xx_gmu_rpmh_bw_votes_init` formula).
2. **rpmh_init at probe without explicit power** (§1.1). Do it with CX and the GMU clocks on. Whether the RSCC/PDC programming survives CX collapse is assumed (Linux relies on it) but unproven.
3. **AHB fence range**: Linux A6xx writes 0x802800A0 (lower = 0xA0). Downstream KGSL is recalled to use lower = 0x8A0 for a6xx; mainline uses 0x8A0 only for a7xx. Unverified. Keep the Linux value, which is known to work on A650 mainline.
4. **HFI table `size` field** is in bytes (120) while the header comment and KGSL say dwords (30). The firmware ignores or tolerates it on mainline. Copy Linux.
5. **Warm boot is unsupported** for A650 in Linux. Every resume reloads all firmware and reruns the full sequence. Do the same.
6. **ACD**: stock enables ACD (`qcom,acd-level` 0x802B5FFD / 0xA02B5FFD per level, plus an AOP mailbox). Mainline does not. Bringing up without ACD matches mainline. Enabling it needs the AOSS QMP message `{class: gpu, res: acd, val: 1}`, then HFI FEATURE_CTRL(12, 1, 0) and the ACD table (`a6xx_gmu.c:1668-1735`, `a6xx_hfi.c:770-798`).
7. **Speed bin**: mainline excludes 670 MHz for fuse 0, but stock allows it. Read the fuse on the target before choosing the table.
8. **Initial level**: Linux boots the GPU at the highest OPP. A more conservative first bring-up could vote a low level first: send GX_BW_PERF_VOTE with freq = 1 right after START.
9. **Firmware blob identity**: the block table in §1.5 comes from the linux-firmware `a650_gmu.bin` (v2.1.8). The device's vendor blob (stock `/vendor/firmware/a650_gmu.bin`) may differ; re-parse it. Any FW version ≤ 0x20010004 switches the init-done check to `INIT_RESULT == 0xBABEFACE`.
10. **SMMU**: the GMU (SID 5) needs its own stage-1 context bank. The GPU uses SID 0/0x401 with a split TTBR. The GMU and GPU must not share a context bank. Code regions need privileged-only, executable mappings (icache, dummy). Data regions (hfi, log) are RW. Re-init the SMMU after each CX GDSC power-on.
11. **GX GDSC**: never enabled by the host. The host only disables it at teardown or failure. Verify how gpucc-sm8250 defines it (it is believed to use `gdsc_gx_do_nothing_enable`).
12. **`a6xx_gmu_pwrlevels_probe` errors are ignored** by Linux. In XNU, treat a cmd-db lookup failure as fatal, because all-zero votes would make the GMU vote the rails off.
