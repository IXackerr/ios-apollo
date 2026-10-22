# Adreno 650 (SM8250) CP / hw_init bring-up specification

Extracted from the Linux msm DRM driver in `apollo-linux` (royka1/apollo-linux, branch apollo-6.17) so that a new XNU driver can be written without the Linux code. Only the A650 path is covered.

* GPU: chip id `0x06050002`. DT compatible `"qcom,adreno-650.2"` gives core 6, major 5, minor 0, patch 2 (`adreno_device.c:146-161`, `sm8250.dtsi:2962`).
* GPU MMIO ("kgsl_3d0_reg_memory"): `0x03d00000`, size `0x40000` (`sm8250.dtsi:2965`). IRQ: GIC SPI 300, level-high (`sm8250.dtsi:2968`). Linux requests it with `IRQF_TRIGGER_HIGH` (`msm_gpu.c:1033`).
* GMU MMIO ("gmu"): `0x03d6a000`, size `0x30000` (`sm8250.dtsi:3036`). GMU register offsets in `a6xx_gmu.xml` are dword offsets from `0x03d6a000`; add `0x1a800` to get the GPU-relative dword offset (see the comment at `a6xx_gpu.c:237-241`).
* GPU SMMU: `0x03da0000`, `qcom,sm8250-smmu-500` / `qcom,adreno-smmu` / `arm,mmu-500`, `dma-coherent`. The GPU uses SID 0 (mask 0x401) (`sm8250.dtsi:2970,3087-3110`).
* Stock DT values used below: `highest_bank_bit = 16`, `ubwc-mode = 4` (UBWC 4.0), `min-access-length = 32` (so `min_acc_len_64b = 0`).

**Citations.** File paths are relative to `drivers/gpu/drm/msm/` unless stated otherwise. `adreno/` is omitted for the adreno files: `a6xx_gpu.c`, `a6xx_gpu.h`, `a6xx_catalog.c`, `adreno_gpu.c`, `adreno_gpu.h`, `adreno_device.c`, `a6xx_gmu.c`, `a6xx_preempt.c`. XML files are under `registers/adreno/`.

**Register addresses.** Register offsets are DWORD offsets. Byte offset = `off*4`. Physical address = `0x03d00000 + off*4`.

**How offsets were computed.** All offsets below were generated from the in-tree XML with the in-tree `registers/gen_header.py` and compiled. They are not hand-copied. The Mesa XML (`mesa/src/freedreno/registers/adreno/`) was cross-checked for the UBWC, protect and CP registers, and the A6XX offsets agree.

---

## 0. A650 configuration facts (catalog and derived flags)

| Item | A650 value | Source |
|---|---|---|
| catalog entry | `chip_ids 0x06050002`, `family ADRENO_6XX_GEN3`, `revn 650` | `a6xx_catalog.c:963-988` |
| firmware | SQE `a650_sqe.fw`, GMU `a650_gmu.bin` | `a6xx_catalog.c:967-970` |
| zap fw | `a650_zap.mdt` (catalog). The DT overrides it with `firmware-name = "postmarketos/a650_zap.mbn"` on apollo. | `a6xx_catalog.c:976`, `sm8250-xiaomi-apollo.dts:612-618` |
| GMEM | `SZ_1M + SZ_128K` = `0x120000` (1179648) | `a6xx_catalog.c:971` |
| quirks | `ADRENO_QUIRK_HAS_CACHED_COHERENT` (bit 4) and `ADRENO_QUIRK_HAS_HW_APRIV` (bit 3). Not set: TWO_PASS_USE_WFI, FAULT_DETECT_MASK, LMLOADKILL_DISABLE, PREEMPTION, 4GB_VA. | `a6xx_catalog.c:973-974`, `adreno_gpu.h:55-61` |
| `hwcg` | `a650_hwcg` | `a6xx_catalog.c:978` |
| `protect` | `a650_protect` (39 entries, `count_max` 48) | `a6xx_catalog.c:979,546-587` |
| `gmu_cgc_mode` | `0x00020202` | `a6xx_catalog.c:980` |
| `prim_fifo_threshold` | `0x00300200` | `a6xx_catalog.c:981` |
| speedbins (fuse to bin) | 0→0, 1→1, 2→3, 3→2 | `a6xx_catalog.c:983-988` |
| `inactive_period` | `DRM_MSM_INACTIVE_PERIOD` = 66 ms (runtime-PM autosuspend) | `msm_gpu.h:249`, `adreno_gpu.c:1241` |
| `gpu->hw_apriv` | **true** (from the quirk) | `a6xx_gpu.c:2506-2507` |
| `a6xx_gpu->has_whereami` | **false**. It is only set in the a630 branch. | `a6xx_gpu.c:913-942` |
| `priv->has_cached_coherent` | **true**. This allows `MSM_BO_CACHED_COHERENT` BOs. | `adreno_device.c:228-229` |
| `allow_relocs` | false (family ≥ 6XX_GEN1) | `adreno_gpu.c:1203` |
| `gmu_is_wrapper` | false (the GMU node is `qcom,adreno-gmu`) | `a6xx_gpu.c:2504`, `sm8250.dtsi:3034` |
| `gmu.legacy` | false (only a630 family and the gmu-wrapper set it) | `a6xx_gmu.c:1871,1996-1998` |
| `have_mmu500` | true (the `iommus` phandle is compatible `arm,mmu-500`) | `a6xx_gpu.c:1914-1917` |
| `nr_rings` | **1**. `enable_preemption` defaults to -1 and A650 lacks `ADRENO_QUIRK_PREEMPTION`, so the code uses `funcs` (`a6xx_submit`). | `adreno_device.c:19`, `a6xx_gpu.c:2523-2531` |
| `uche_trap_base` | `0x0001_FFFF_FFFF_F000` | `a6xx_gpu.c:2562` |
| `has_ray_tracing` | false (only set for a740/a750) | `a6xx_gpu.c:1972-1975` |
| family helper predicates | `a650_family` = true. `a660_family` = false. `a640_family` = false. `a610_family` = false. `a7xx` = false. `a6xx_has_gbif` = true. | `adreno_gpu.h:487-548`, `a6xx_gpu.h:218-224` |

**Warning.** Setting the module parameter `enable_preemption=1` forces `funcs_a7xx` with 4 rings even on A650 (`a6xx_gpu.c:2523-2525`). That path emits `CP_THREAD_CONTROL` and other A7xx packets. Never mirror it.

### 0.1 UBWC config (derived values)

`a6xx_calc_ubwc_config()` (`a6xx_gpu.c:607-675`) starts from the SoC common UBWC table (`qcom_ubwc_config_get_data()`) and then forces two values:

* `ubwc_swizzle = 0x6` (`:620`)
* `highest_bank_bit = 16` for A650 (`:644-651`)

The SoC table source (`drivers/soc/qcom/ubwc_config.c`) is **not in this tree**. The upstream `sm8250_data` contains:

* `ubwc_enc_version = UBWC_4_0`
* `ubwc_swizzle = LVL2|LVL3` (0x6)
* `highest_bank_bit = 16`
* `macrotile_mode = true`

These match the stated DT values (ubwc-mode 4, hbb 16).

**Verify this** against your ubwc_config.c. The `macrotile_mode` value feeds `RBBM_NC_MODE_CNTL` and `MSM_PARAM_MACROTILE_MODE`.

Inputs to `a6xx_set_ubwc_config` (`a6xx_gpu.c:677-730`):

| Variable | Formula | A650 value |
|---|---|---|
| `hbb` | `highest_bank_bit - 13` | 3 |
| `hbb_hi` | `hbb >> 2` | 0 |
| `hbb_lo` | `hbb & 3` | 3 |
| `rgb565_predicator` | `enc ≥ UBWC_4_0` | 1 |
| `level2_swizzling_dis` | `!(0x6 & LVL2(bit1))` | 0 |
| `ubwc_mode` | `enc == UBWC_1_0` | 0 |
| `amsbc` | `enc ≥ UBWC_3_0` | 1 |
| `min_acc_len_64b` | only true for a610/a702 | 0 |
| `uavflagprd_inv` | 2 for the a650 family (`:697-698`) | 2 |

---

## 1. `a6xx_hw_init()` for A650: ordered register sequence

### 1.0 Call path and preconditions

**Order of operations at load:**

1. `adreno_load_gpu()` → `adreno_load_fw()` (reads the files).
2. `gpu->funcs->ucode_load()` = `a6xx_ucode_load` (creates the SQE BO, checks its version, allocates the shadow BO). See §2.
3. `pm_runtime_get_sync()` → `a6xx_gmu_pm_resume()`.
4. `msm_gpu_hw_init()` → `a6xx_hw_init()` (`adreno_device.c:86-115`).

**IRQ masking.** `msm_gpu_hw_init` disables the GPU IRQ around `hw_init` (`msm_gpu.c:159-175`).

**Re-init on every runtime resume.** `hw_init` re-runs on **every** runtime resume, because `needs_hw_init = true` is set in `a6xx_gmu_pm_resume` (`a6xx_gpu.c:2046`). The resume is lazily triggered from `msm_gpu_submit` (`msm_gpu.c:885`). Autosuspend happens after 66 ms idle. Each re-init resets the ring to its start (`adreno_gpu.c:676-677`) and restarts the SQE.

**Precondition (not specified here): GMU booted and GX power rail on.** This is done by `a6xx_gmu_resume()` in `a6xx_gmu.c`. Right after it, `a6xx_gmu_pm_resume` calls `a6xx_llc_activate` (`a6xx_gpu.c:2058`). For A650 with an MMU-500, that function:

* does nothing if no LLCC slices exist (`a6xx_gpu.c:1924-1928,1808-1809,1839-1840`);
* otherwise does `gpu_rmw(GBIF_SCACHE_CNTL1 [0x03c02], GENMASK(24,0), scid | scid<<5 | scid<<10 | scid<<15 | scid<<20)` (`a6xx_gpu.c:1811-1816,1859`).

The HTW slice is not programmed because `have_mmu500` is set (`:1830-1836`).

**Recommendation for XNU: skip LLC.** Skipping it is a supported Linux configuration.

**Locking.** The whole `hw_init` runs under `gmu.lock` (`a6xx_gpu.c:1422-1433`).

### 1.1 Ordered steps

Legend:

* `W` = 32-bit write.
* `W64` = write LO at `off`, then HI at `off+1` (`msm_gpu.h:653-658`).
* `R` = read (used as a posting read).
* `G:` = GMU register. The physical address is `0x03d6a000 + off*4`.

| # | Op | Register | dword off | byte off | phys | Value | Source |
|---|---|---|---|---|---|---|---|
| 1 | G:W | GMU_HOST2GMU_INTR_SET | 0x05194 | 0x14650 | 0x03d7e650 | `1<<30` (OOB GPU_SET, non-legacy `set_new`=30) | `a6xx_gpu.c:1073-1078`, `a6xx_gmu.c:301-310,353-364` |
| 2 | G:poll | GMU_GMU2HOST_INTR_INFO | 0x05192 | 0x14648 | 0x03d7e648 | wait for `val & (1<<31)` (`ack_new`=31), 100 µs interval, 10 ms timeout | `a6xx_gmu.c:367-368` |
| 3 | G:W | GMU_GMU2HOST_INTR_CLR | 0x05191 | 0x14644 | 0x03d7e644 | `1<<31` | `a6xx_gmu.c:377` |
| 4 | W | GBIF_HALT | 0x03c45 | 0x0f114 | 0x03d0f114 | 0 | `a6xx_gpu.c:1087-1089` |
| 5 | R | GBIF_HALT | 0x03c45 | | | (posting read) | `:1089` |
| 6 | W | RBBM_GBIF_HALT | 0x00016 | 0x00058 | 0x03d00058 | 0 | `:1091` |
| 7 | R | RBBM_GBIF_HALT | 0x00016 | | | (posting read) | `:1092` |
| 8 | W | RBBM_SECVID_TSB_CNTL | 0x0f803 | 0x3e00c | 0x03d3e00c | 0 | `:1095` |
| 9 | W64 | RBBM_SECVID_TSB_TRUSTED_BASE | 0x0f800/0x0f801 | 0x3e000 | 0x03d3e000 | 0 / 0 | `:1105` |
| 10 | W | RBBM_SECVID_TSB_TRUSTED_SIZE | 0x0f802 | 0x3e008 | 0x03d3e008 | 0 | `:1106` |
| 11 | W | CP_ADDR_MODE_CNTL | 0x00842 | 0x02108 | 0x03d02108 | 1 | `:1110` |
| 12 | W | VSC_ADDR_MODE_CNTL | 0x00c01 | 0x03004 | 0x03d03004 | 1 | `:1111` |
| 13 | W | GRAS_ADDR_MODE_CNTL | 0x08601 | 0x21804 | 0x03d21804 | 1 | `:1112` |
| 14 | W | RB_ADDR_MODE_CNTL | 0x08e05 | 0x23814 | 0x03d23814 | 1 | `:1113` |
| 15 | W | PC_ADDR_MODE_CNTL | 0x09e01 | 0x27804 | 0x03d27804 | 1 | `:1114` |
| 16 | W | HLSQ_ADDR_MODE_CNTL | 0x0be05 | 0x2f814 | 0x03d2f814 | 1 | `:1115` |
| 17 | W | VFD_ADDR_MODE_CNTL | 0x0a601 | 0x29804 | 0x03d29804 | 1 | `:1116` |
| 18 | W | VPC_ADDR_MODE_CNTL | 0x09601 | 0x25804 | 0x03d25804 | 1 | `:1117` |
| 19 | W | UCHE_ADDR_MODE_CNTL | 0x00e00 | 0x03800 | 0x03d03800 | 1 | `:1118` |
| 20 | W | SP_ADDR_MODE_CNTL | 0x0ae01 | 0x2b804 | 0x03d2b804 | 1 | `:1119` |
| 21 | W | TPL1_ADDR_MODE_CNTL | 0x0b601 | 0x2d804 | 0x03d2d804 | 1 | `:1120` |
| 22 | W | RBBM_SECVID_TSB_ADDR_MODE_CNTL | 0x0f810 | 0x3e040 | 0x03d3e040 | 1 | `:1121` |
| 23 | — | **a6xx_set_hwcg(true)**, see §1.2 | | | | | `:1125`, `:511-580` |
| 24 | W | GBIF_QSB_SIDE0 | 0x03c03 | 0x0f00c | 0x03d0f00c | 0x00071620 | `:1128-1132` |
| 25 | W | GBIF_QSB_SIDE1 | 0x03c04 | 0x0f010 | 0x03d0f010 | 0x00071620 | `:1133` |
| 26 | W | GBIF_QSB_SIDE2 | 0x03c05 | 0x0f014 | 0x03d0f014 | 0x00071620 | `:1134` |
| 27 | W | GBIF_QSB_SIDE3 | 0x03c06 | 0x0f018 | 0x03d0f018 | 0x00071620 | `:1135` |
| 28 | W | RBBM_GBIF_CLIENT_QOS_CNTL | 0x00011 | 0x00044 | 0x03d00044 | 0x3 | `:1136-1137` |
| 29 | W | RBBM_PERFCTR_GPU_BUSY_MASKED | 0x0050b | 0x0142c | 0x03d0142c | 0xffffffff | `:1149` |
| 30 | W64 | UCHE_WRITE_RANGE_MAX | 0x00e05/0x00e06 | 0x03814 | 0x03d03814 | `uche_trap_base+0xfc0` = 0x1_FFFF_FFFF_FFC0 (LO 0xffffffc0, HI 0x0001ffff) | `:1156` |
| 31 | W64 | UCHE_TRAP_BASE | 0x00e09/0x00e0a | 0x03824 | 0x03d03824 | 0x1_FFFF_FFFF_F000 (LO 0xfffff000, HI 0x0001ffff) | `:1157` |
| 32 | W64 | UCHE_WRITE_THRU_BASE | 0x00e07/0x00e08 | 0x0381c | 0x03d0381c | 0x1_FFFF_FFFF_F000 | `:1158` |
| — | skip | UCHE_GMEM_RANGE_MIN/MAX | | | | **not written for the a650 family** | `:1161-1171` |
| 33 | W | UCHE_FILTER_CNTL | 0x00e18 | 0x03860 | 0x03d03860 | 0x804 | `:1176` |
| 34 | W | UCHE_CACHE_WAYS | 0x00e17 | 0x0385c | 0x03d0385c | 0x4 | `:1177` |
| 35 | W | CP_ROQ_THRESHOLDS_2 | 0x008c2 | 0x02308 | 0x03d02308 | 0x02000140 | `:1180-1181` |
| 36 | W | CP_ROQ_THRESHOLDS_1 | 0x008c1 | 0x02304 | 0x03d02304 | 0x8040362c | `:1182` |
| — | skip | CP_LPAC_PROG_FIFO_SIZE | | | | a660 family only | `:1191-1192` |
| 37 | W | CP_MEM_POOL_SIZE | 0x008c3 | 0x0230c | 0x03d0230c | 128 (0x80) | `:1201-1202` |
| 38 | W | PC_DBG_ECO_CNTL | 0x09e00 | 0x27800 | 0x03d27800 | 0x00300200 (`prim_fifo_threshold`) | `:1206-1208` |
| 39 | W | CP_AHB_CNTL | 0x0098d | 0x02634 | 0x03d02634 | 0x1 (AHB default slave response = ERROR) | `:1211` |
| 40 | W | RBBM_PERFCTR_CNTL | 0x00500 | 0x01400 | 0x03d01400 | 0x1 | `:1214` |
| 41 | W | CP_PERFCTR_CP_SEL(0) | 0x008d0 | 0x02340 | 0x03d02340 | 0 (`PERF_CP_ALWAYS_COUNT`) | `:1223`, `a6xx_perfcntrs.xml` |
| 42 | W | RB_NC_MODE_CNTL | 0x08e08 | 0x23820 | 0x03d23820 | **0x00000816** = `l2dis<<12 \| rgb565<<11 \| hbb_hi<<10 \| amsbc<<4 \| min64<<3 \| hbb_lo<<1 \| ubwc_mode` = `0<<12\|1<<11\|0<<10\|1<<4\|0<<3\|3<<1\|0` | `:703-708` |
| 43 | W | TPL1_NC_MODE_CNTL | 0x0b604 | 0x2d810 | 0x03d2d810 | **0x00000006** = `l2dis<<6 \| hbb_hi<<4 \| min64<<3 \| hbb_lo<<1 \| ubwc_mode` | `:710-713` |
| 44 | W | SP_NC_MODE_CNTL | 0x0ae02 | 0x2b808 | 0x03d2b808 | **0x00000026** = `l2dis<<12 \| hbb_hi<<10 \| uavflagprd_inv(2)<<4 \| min64<<3 \| hbb_lo<<1 \| ubwc_mode` | `:715-719` |
| 45 | W | UCHE_MODE_CNTL | 0x00e01 | 0x03804 | 0x03d03804 | **0x00600000** = `min64<<23 \| hbb_lo<<21` | `:725-726` |
| 46 | W | RBBM_NC_MODE_CNTL | 0x00534 | 0x014d0 | 0x03d014d0 | `macrotile_mode` = **1** (sm8250 table, see §0.1) | `:728-729` |
| 47 | W | RBBM_INTERFACE_HANG_INT_CNTL | 0x0001f | 0x0007c | 0x03d0007c | `(1<<30) \| 0x1fffff` = **0x401fffff** (the "else" branch) | `:1237-1238` |
| 48 | W | UCHE_CLIENT_PF | 0x00e19 | 0x03864 | 0x03d03864 | `BIT(7)\|1` = 0x81 | `:1240` |
| 49 | W | TPL1_BICUBIC_WEIGHTS_TABLE_0 | 0x0b608 | 0x2d820 | 0x03d2d820 | 0 | `:1243-1244` |
| 50 | W | TPL1_BICUBIC_WEIGHTS_TABLE_1 | 0x0b609 | 0x2d824 | 0x03d2d824 | 0x3fe05ff4 | `:1245-1246` |
| 51 | W | TPL1_BICUBIC_WEIGHTS_TABLE_2 | 0x0b60a | 0x2d828 | 0x03d2d828 | 0x3fa0ebee | `:1247-1248` |
| 52 | W | TPL1_BICUBIC_WEIGHTS_TABLE_3 | 0x0b60b | 0x2d82c | 0x03d2d82c | 0x3f5193ed | `:1249-1250` |
| 53 | W | TPL1_BICUBIC_WEIGHTS_TABLE_4 | 0x0b60c | 0x2d830 | 0x03d2d830 | 0x3f0243f0 | `:1251-1252` |
| 54 | G:W | GPU_GMU_AO_GPU_CX_BUSY_MASK | 0x0930e | 0x24c38 | 0x03d8ec38 | 0xff000000 | `:1256` |
| 55 | G:RMW | GMU_CX_GMU_POWER_COUNTER_SELECT_0 | 0x05041 | 0x14104 | 0x03d7e104 | `(v & ~0xff) \| 0x20` | `:1259`, `a6xx_gmu.h:139-146` |
| 56 | G:W | GMU_CX_GMU_POWER_COUNTER_ENABLE | 0x05040 | 0x14100 | 0x03d7e100 | 1 | `:1260` |
| 57 | — | **a6xx_set_cp_protect()**, see §1.3 | | | | | `:1263`, `:582-605` |
| — | skip | CP_CHICKEN_DBG, RBBM_GBIF_CLIENT_QOS_CNTL=0 | | | | a660 family only | `:1265-1274` |
| — | skip | UCHE_CMDQ_CONFIG | | | | a660/a663/a690/a7xx only | `:1276-1287` |
| 58 | W | CP_APRIV_CNTL | 0x00844 | 0x02110 | 0x03d02110 | `BIT6\|BIT5\|BIT3\|BIT2\|BIT1` = **0x0000006e**: b6 CDWRITE (crashdumper writes), b5 CDREAD (crashdumper reads), b3 RBRPWB (RPTR shadow writes), b2 RBPRIVLEVEL (memory accesses from PM4 packets in the ringbuffer), b1 RBFETCH (ringbuffer reads). b0 ICACHE (instruction cache fetches) is **not** set. | `:1290-1300`, `a6xx.xml:102-117` |
| 59 | W | RBBM_INT_0_MASK | 0x00038 | 0x000e0 | 0x03d000e0 | `A6XX_INT_MASK` = **0x03d0e242** (§6) | `:1315-1316`, `:1028-1038` |
| 60 | — | **adreno_hw_init()**, see §1.4 | | | | | `:1318`, `adreno_gpu.c:655-691` |
| 61 | W64 | CP_SQE_INSTR_BASE | 0x00830/0x00831 | 0x020c0 | 0x03d020c0 | `sqe_iova` (§2) | `:1322` |
| 62 | W64 | CP_RB_BASE | 0x00800/0x00801 | 0x02000 | 0x03d02000 | `rb[0]->iova` | `:1325` |
| 63 | W | CP_RB_CNTL | 0x00802 | 0x02008 | 0x03d02008 | `MSM_GPU_RB_CNTL_DEFAULT` = **0x0000020c**. NO_UPDATE is *not* set because `hw_apriv` (§3). | `:1331-1335` |
| 64 | W64 | CP_RB_RPTR_ADDR | 0x00804/0x00805 | 0x02010 | 0x03d02010 | `shadow_iova + ring_id*4` (ring 0 gives `shadow_iova`). Then the CPU zeroes `shadow[0]`. | `:1338-1343`, `a6xx_gpu.h:241-242` |
| — | no-op | a6xx_preempt_hw_init | | | | returns early because `nr_rings == 1` | `:1351`, `a6xx_preempt.c:194-196` |
| 65 | (sw) | `cur_ring = rb[0]`, `rb[i]->cur_ctx_seqno = 0` | | | | forces a pagetable switch on the next submit | `:1354-1357` |
| 66 | W | CP_SQE_CNTL | 0x00808 | 0x02020 | 0x03d02020 | 1 (start SQE) | `:1360` |
| 67 | — | **a6xx_cp_init()**: emit CP_ME_INIT, write WPTR, wait idle (§4.1) | | | | | `:1367`, `:803-830` |
| 68 | — | **Zap / secure-mode exit** (§4.2): either a CP_SET_SECURE_MODE packet or `RBBM_SECVID_TRUST_CNTL[0x0f400]=0` | | | | | `:1378-1399` |
| — | no-op | a7xx_preempt_start | | | | returns 0 because `nr_rings <= 1` | `:1406`, `:781-782` |
| 69 | G:W | GMU_HOST2GMU_INTR_SET | 0x05194 | | 0x03d7e650 | `1<<31` (OOB GPU_SET clear, `clear_new`=31) | `:1412`, `a6xx_gmu.c:383-398` |
| — | skip | clear OOB BOOT_SLUMBER | | | | only if `gmu.legacy` (false) | `:1414-1417` |

**Not done on A650 in `hw_init`:**

* `RBBM_SW_RESET_CMD`. Reset comes from GX power collapse; a soft reset happens only for the gmu-wrapper in `recover`, `a6xx_gpu.c:1479-1486`.
* `VBIF_GATE_OFF_WRREQ_EN` (a630 only).
* `UCHE_GBIF_GX_CONFIG` (a7xx only).
* `RBBM_VBIF_CLIENT_QOS_CNTL` (non-GBIF only).

### 1.2 `a6xx_set_hwcg(gpu, true)` for A650 (`a6xx_gpu.c:511-580`)

1. G:W `GPU_GMU_AO_GMU_CGC_MODE_CNTL` (G 0x09309, phys 0x03d8ec24) = `gmu_cgc_mode` = **0x00020202** (`:536-537`).
2. G:W `GPU_GMU_AO_GMU_CGC_DELAY_CNTL` (G 0x0930a, phys 0x03d8ec28) = **0x10111**. A650 is not in the a615 family (`:533,538-539`).
3. G:W `GPU_GMU_AO_GMU_CGC_HYST_CNTL` (G 0x0930b, phys 0x03d8ec2c) = **0x5555** (`:534,540-541`).
4. R `RBBM_CLOCK_CNTL` (0x000ae, phys 0x03d002b8). If the value is already `0x8aa8aa82`, **return here** (`:562-566`). `clock_cntl_on` = 0x8aa8aa82 for A650 (`:524-531`).
5. G:RMW `GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL` (G 0x00080, phys 0x03d6a200): clear bit 0 (`:569-570`).
6. Write every entry of `a650_hwcg` in order. The table is copied literally from `a6xx_catalog.c:347-398`, with offsets resolved:

| i | Register | dword | byte | value |
|---|---|---|---|---|
| 0 | RBBM_CLOCK_CNTL_SP0 | 0x000b0 | 0x002c0 | 0x02222222 |
| 1 | RBBM_CLOCK_CNTL2_SP0 | 0x000b4 | 0x002d0 | 0x02222220 |
| 2 | RBBM_CLOCK_DELAY_SP0 | 0x000b8 | 0x002e0 | 0x00000080 |
| 3 | RBBM_CLOCK_HYST_SP0 | 0x000bc | 0x002f0 | 0x0000F3CF |
| 4 | RBBM_CLOCK_CNTL_TP0 | 0x000c0 | 0x00300 | 0x02222222 |
| 5 | RBBM_CLOCK_CNTL2_TP0 | 0x000c4 | 0x00310 | 0x22222222 |
| 6 | RBBM_CLOCK_CNTL3_TP0 | 0x000c8 | 0x00320 | 0x22222222 |
| 7 | RBBM_CLOCK_CNTL4_TP0 | 0x000cc | 0x00330 | 0x00022222 |
| 8 | RBBM_CLOCK_DELAY_TP0 | 0x000d0 | 0x00340 | 0x11111111 |
| 9 | RBBM_CLOCK_DELAY2_TP0 | 0x000d4 | 0x00350 | 0x11111111 |
| 10 | RBBM_CLOCK_DELAY3_TP0 | 0x000d8 | 0x00360 | 0x11111111 |
| 11 | RBBM_CLOCK_DELAY4_TP0 | 0x000dc | 0x00370 | 0x00011111 |
| 12 | RBBM_CLOCK_HYST_TP0 | 0x000e0 | 0x00380 | 0x77777777 |
| 13 | RBBM_CLOCK_HYST2_TP0 | 0x000e4 | 0x00390 | 0x77777777 |
| 14 | RBBM_CLOCK_HYST3_TP0 | 0x000e8 | 0x003a0 | 0x77777777 |
| 15 | RBBM_CLOCK_HYST4_TP0 | 0x000ec | 0x003b0 | 0x00077777 |
| 16 | RBBM_CLOCK_CNTL_RB0 | 0x000f0 | 0x003c0 | 0x22222222 |
| 17 | RBBM_CLOCK_CNTL2_RB0 | 0x000f4 | 0x003d0 | 0x01002222 |
| 18 | RBBM_CLOCK_CNTL_CCU0 | 0x000f8 | 0x003e0 | 0x00002220 |
| 19 | RBBM_CLOCK_HYST_RB_CCU0 | 0x00100 | 0x00400 | 0x00040F00 |
| 20 | RBBM_CLOCK_CNTL_RAC | 0x00104 | 0x00410 | 0x25222022 |
| 21 | RBBM_CLOCK_CNTL2_RAC | 0x00105 | 0x00414 | 0x00005555 |
| 22 | RBBM_CLOCK_DELAY_RAC | 0x00106 | 0x00418 | 0x00000011 |
| 23 | RBBM_CLOCK_HYST_RAC | 0x00107 | 0x0041c | 0x00445044 |
| 24 | RBBM_CLOCK_CNTL_TSE_RAS_RBBM | 0x00108 | 0x00420 | 0x04222222 |
| 25 | RBBM_CLOCK_MODE_VFD | 0x00111 | 0x00444 | 0x00002222 |
| 26 | RBBM_CLOCK_MODE_GPC | 0x00114 | 0x00450 | 0x00222222 |
| 27 | RBBM_CLOCK_DELAY_HLSQ_2 | 0x00117 | 0x0045c | 0x00000002 |
| 28 | RBBM_CLOCK_MODE_HLSQ | 0x0011b | 0x0046c | 0x00002222 |
| 29 | RBBM_CLOCK_DELAY_TSE_RAS_RBBM | 0x00109 | 0x00424 | 0x00004000 |
| 30 | RBBM_CLOCK_DELAY_VFD | 0x00112 | 0x00448 | 0x00002222 |
| 31 | RBBM_CLOCK_DELAY_GPC | 0x00115 | 0x00454 | 0x00000200 |
| 32 | RBBM_CLOCK_DELAY_HLSQ | 0x0011c | 0x00470 | 0x00000000 |
| 33 | RBBM_CLOCK_HYST_TSE_RAS_RBBM | 0x0010a | 0x00428 | 0x00000000 |
| 34 | RBBM_CLOCK_HYST_VFD | 0x00113 | 0x0044c | 0x00000000 |
| 35 | RBBM_CLOCK_HYST_GPC | 0x00116 | 0x00458 | 0x04104004 |
| 36 | RBBM_CLOCK_HYST_HLSQ | 0x0011d | 0x00474 | 0x00000000 |
| 37 | RBBM_CLOCK_CNTL_TEX_FCHE | 0x00120 | 0x00480 | 0x00000222 |
| 38 | RBBM_CLOCK_DELAY_TEX_FCHE | 0x00121 | 0x00484 | 0x00000111 |
| 39 | RBBM_CLOCK_HYST_TEX_FCHE | 0x00122 | 0x00488 | 0x00000777 |
| 40 | RBBM_CLOCK_CNTL_UCHE | 0x0010b | 0x0042c | 0x22222222 |
| 41 | RBBM_CLOCK_HYST_UCHE | 0x00110 | 0x00440 | 0x00000004 |
| 42 | RBBM_CLOCK_DELAY_UCHE | 0x0010f | 0x0043c | 0x00000002 |
| 43 | RBBM_ISDB_CNT | 0x00533 | 0x014cc | 0x00000182 |
| 44 | RBBM_RAC_THRESHOLD_CNT | 0x00044 | 0x00110 | 0x00000000 |
| 45 | RBBM_SP_HYST_CNT | 0x00042 | 0x00108 | 0x00000000 |
| 46 | RBBM_CLOCK_CNTL_GMU_GX | 0x00118 | 0x00460 | 0x00000222 |
| 47 | RBBM_CLOCK_DELAY_GMU_GX | 0x00119 | 0x00464 | 0x00000111 |
| 48 | RBBM_CLOCK_HYST_GMU_GX | 0x0011a | 0x00468 | 0x00000555 |

The table holds 49 writes and is terminated by `{}` (offset 0). The physical address of each entry is `0x03d00000 + byte`. `RBBM_CLOCK_CNTL` is **not** in the A650 table; it is written last (step 8).

7. G:RMW `GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL`: set bit 0 (`:576-577`).
8. W `RBBM_CLOCK_CNTL` (0x000ae, phys 0x03d002b8) = **0x8aa8aa82** (`:579`).

### 1.3 `a6xx_set_cp_protect()` for A650 (`a6xx_gpu.c:582-605`)

**Control register.** Write `CP_PROTECT_CNTL` (0x0084f, phys 0x03d0213c) = `ACCESS_PROT_EN(b0) | ACCESS_FAULT_ON_VIOL_EN(b1) | LAST_SPAN_INF_RANGE(b3)` = **0x0000000b** (`:593-596`).

**Entry encoding.** Each entry has the `a6x_cp_protect` layout (`a6xx_enums.xml:191-194`):

* `BASE_ADDR[17:0]` (register dword offset)
* `MASK_LEN[30:18]`
* `READ[31]` (1 = reads are also blocked)

It protects `len+1` registers starting at `reg` (`a6xx_gpu.h:201-216`). Two macro forms exist:

* `NORDWR(reg,len) = 1<<31 | (len&0x3fff)<<18 | reg`
* `RDONLY(reg,len) = (len&0x3fff)<<18 | reg`

**Count and last span.** `count = 39` (`ARRAY_SIZE`), `count_max = 48` (`DECLARE_ADRENO_PROTECT(a650_protect, 48)`, `a6xx_catalog.c:587`).

* Entries `i = 0..37` (`count-1`) are written to `CP_PROTECT(i)`. The loop skips any zero entries; A650 has none.
* The final entry (`regs[38]`) is written to `CP_PROTECT(count_max-1)` = **`CP_PROTECT(47)`**. Because LAST_SPAN_INF_RANGE is set, it extends from `0x1f8c0` to the end of the register space.
* `CP_PROTECT(38..46)` are **never written**; they are left at their reset value.

**XML caveat.** The XML declares the `CP_PROTECT` array with `length="32"` (`a6xx.xml:163`, Mesa `a6xx.xml:293`). The hardware and Linux use index 47 (`0x0087f`), so the XML length is stale.

| i | Register written | dword | phys | macro | value | protects |
|---|---|---|---|---|---|---|
| 0 | CP_PROTECT(0) | 0x00850 | 0x03d02140 | RDONLY(0x00000, 0x04ff) | 0x13FC0000 | 0x00000-0x004ff (writes) |
| 1 | CP_PROTECT(1) | 0x00851 | 0x03d02144 | RDONLY(0x00501, 0x0005) | 0x00140501 | 0x00501-0x00506 |
| 2 | CP_PROTECT(2) | 0x00852 | 0x03d02148 | RDONLY(0x0050b, 0x02f4) | 0x0BD0050B | 0x0050b-0x007ff |
| 3 | CP_PROTECT(3) | 0x00853 | 0x03d0214c | NORDWR(0x0050e, 0x0000) | 0x8000050E | 0x0050e |
| 4 | CP_PROTECT(4) | 0x00854 | 0x03d02150 | NORDWR(0x00510, 0x0000) | 0x80000510 | 0x00510 |
| 5 | CP_PROTECT(5) | 0x00855 | 0x03d02154 | NORDWR(0x00534, 0x0000) | 0x80000534 | 0x00534 |
| 6 | CP_PROTECT(6) | 0x00856 | 0x03d02158 | NORDWR(0x00800, 0x0082) | 0x82080800 | 0x00800-0x00882 |
| 7 | CP_PROTECT(7) | 0x00857 | 0x03d0215c | NORDWR(0x008a0, 0x0008) | 0x802008A0 | 0x008a0-0x008a8 |
| 8 | CP_PROTECT(8) | 0x00858 | 0x03d02160 | NORDWR(0x008ab, 0x0024) | 0x809008AB | 0x008ab-0x008cf |
| 9 | CP_PROTECT(9) | 0x00859 | 0x03d02164 | RDONLY(0x008de, 0x00ae) | 0x02B808DE | 0x008de-0x0098c |
| 10 | CP_PROTECT(10) | 0x0085a | 0x03d02168 | NORDWR(0x00900, 0x004d) | 0x81340900 | 0x00900-0x0094d |
| 11 | CP_PROTECT(11) | 0x0085b | 0x03d0216c | NORDWR(0x0098d, 0x0272) | 0x89C8098D | 0x0098d-0x00bff |
| 12 | CP_PROTECT(12) | 0x0085c | 0x03d02170 | NORDWR(0x00e00, 0x0001) | 0x80040E00 | 0x00e00-0x00e01 |
| 13 | CP_PROTECT(13) | 0x0085d | 0x03d02174 | NORDWR(0x00e03, 0x000c) | 0x80300E03 | 0x00e03-0x00e0f |
| 14 | CP_PROTECT(14) | 0x0085e | 0x03d02178 | NORDWR(0x03c00, 0x00c3) | 0x830C3C00 | 0x03c00-0x03cc3 |
| 15 | CP_PROTECT(15) | 0x0085f | 0x03d0217c | RDONLY(0x03cc4, 0x1fff) | 0x7FFC3CC4 | 0x03cc4-0x05cc3 |
| 16 | CP_PROTECT(16) | 0x00860 | 0x03d02180 | NORDWR(0x08630, 0x01cf) | 0x873C8630 | 0x08630-0x087ff |
| 17 | CP_PROTECT(17) | 0x00861 | 0x03d02184 | NORDWR(0x08e00, 0x0000) | 0x80008E00 | 0x08e00 |
| 18 | CP_PROTECT(18) | 0x00862 | 0x03d02188 | NORDWR(0x08e08, 0x0000) | 0x80008E08 | 0x08e08 |
| 19 | CP_PROTECT(19) | 0x00863 | 0x03d0218c | NORDWR(0x08e50, 0x001f) | 0x807C8E50 | 0x08e50-0x08e6f |
| 20 | CP_PROTECT(20) | 0x00864 | 0x03d02190 | NORDWR(0x08e80, 0x027f) | 0x89FC8E80 | 0x08e80-0x090ff |
| 21 | CP_PROTECT(21) | 0x00865 | 0x03d02194 | NORDWR(0x09624, 0x01db) | 0x876C9624 | 0x09624-0x097ff |
| 22 | CP_PROTECT(22) | 0x00866 | 0x03d02198 | NORDWR(0x09e60, 0x0011) | 0x80449E60 | 0x09e60-0x09e71 |
| 23 | CP_PROTECT(23) | 0x00867 | 0x03d0219c | NORDWR(0x09e78, 0x0187) | 0x861C9E78 | 0x09e78-0x09fff |
| 24 | CP_PROTECT(24) | 0x00868 | 0x03d021a0 | NORDWR(0x0a630, 0x01cf) | 0x873CA630 | 0x0a630-0x0a7ff |
| 25 | CP_PROTECT(25) | 0x00869 | 0x03d021a4 | NORDWR(0x0ae02, 0x0000) | 0x8000AE02 | 0x0ae02 |
| 26 | CP_PROTECT(26) | 0x0086a | 0x03d021a8 | NORDWR(0x0ae50, 0x032f) | 0x8CBCAE50 | 0x0ae50-0x0b17f |
| 27 | CP_PROTECT(27) | 0x0086b | 0x03d021ac | NORDWR(0x0b604, 0x0000) | 0x8000B604 | 0x0b604 |
| 28 | CP_PROTECT(28) | 0x0086c | 0x03d021b0 | NORDWR(0x0b608, 0x0007) | 0x801CB608 | 0x0b608-0x0b60f |
| 29 | CP_PROTECT(29) | 0x0086d | 0x03d021b4 | NORDWR(0x0be02, 0x0001) | 0x8004BE02 | 0x0be02-0x0be03 |
| 30 | CP_PROTECT(30) | 0x0086e | 0x03d021b8 | NORDWR(0x0be20, 0x17df) | 0xDF7CBE20 | 0x0be20-0x0d5ff |
| 31 | CP_PROTECT(31) | 0x0086f | 0x03d021bc | NORDWR(0x0f000, 0x0bff) | 0xAFFCF000 | 0x0f000-0x0fbff |
| 32 | CP_PROTECT(32) | 0x00870 | 0x03d021c0 | RDONLY(0x0fc00, 0x1fff) | 0x7FFCFC00 | 0x0fc00-0x11bff |
| 33 | CP_PROTECT(33) | 0x00871 | 0x03d021c4 | NORDWR(0x18400, 0x1fff) | 0xFFFD8400 | 0x18400-0x1a3ff |
| 34 | CP_PROTECT(34) | 0x00872 | 0x03d021c8 | NORDWR(0x1a800, 0x1fff) | 0xFFFDA800 | 0x1a800-0x1c7ff (GMU) |
| 35 | CP_PROTECT(35) | 0x00873 | 0x03d021cc | NORDWR(0x1f400, 0x0443) | 0x910DF400 | 0x1f400-0x1f843 |
| 36 | CP_PROTECT(36) | 0x00874 | 0x03d021d0 | RDONLY(0x1f844, 0x007b) | 0x01EDF844 | 0x1f844-0x1f8bf |
| 37 | CP_PROTECT(37) | 0x00875 | 0x03d021d4 | NORDWR(0x1f887, 0x001b) | 0x806DF887 | 0x1f887-0x1f8a2 |
| 38 | **CP_PROTECT(47)** | 0x0087f | 0x03d021fc | NORDWR(0x1f8c0, 0x0000) | 0x8001F8C0 | 0x1f8c0 → end (LAST_SPAN_INF_RANGE) |

**Consequences.**

* `RBBM_PERFCTR_SRAM_INIT_CMD` (0x50e) is protected (entry 3). This is why the pagetable-switch sequence turns protected mode off (§5.3).
* `RBBM_SECVID_TRUST_CNTL` (0xf400) is covered by entry 31. This only blocks CP (ringbuffer) writes, not CPU MMIO writes.

### 1.4 `adreno_hw_init()` (`adreno_gpu.c:655-691`)

**SMMU aperture SCM call.** If `family ≥ 6XX_GEN1` and `qcom_scm_set_gpu_smmu_aperture_is_available()`, Linux calls `qcom_scm_set_gpu_smmu_aperture(0)` (`:662-668`). This is the "CP SMMU aperture" call. It tells TrustZone to let the CP program SMMU context bank 0. The SMC encoding (`drivers/firmware/qcom/qcom_scm.c:912-934`, `qcom_scm.h:116,123`, `qcom_scm-smc.c:165-172`) is:

* SIP owner, std call, SMC64. FNID = `(svc 0x0c << 8) | cmd 0x1b`.
* `x0 = 0x42000C1B`
* `x1` = arginfo `QCOM_SCM_ARGS(4)` = `0x4`
* `x2 = 0xffff0000 | (cb & 0xff)` = `0xffff0000`
* `x3 = x4 = x5 = 0xffffffff`

Availability is probed first with the SCM "is call available" query. A failure is only logged; it does not stop init.

**Per-ring software reset.** For each ring:

* `cur = next = start`
* `memptrs->rptr = 0`
* `memptrs->bv_fence = fctx->completed_fence`
* If `last_fence` is before `memptrs->fence`, clamp `memptrs->fence = last_fence` (`:670-688`).

---

## 2. SQE firmware (`a650_sqe.fw`)

**Loading the file.** The file is `qcom/a650_sqe.fw`. Linux first tries `/lib/firmware/qcom/<name>`, then the legacy `/lib/firmware/<name>`, then the usermode helper (`adreno_gpu.c:527-606`).

**Placing it in GPU memory** (`adreno_fw_create_bo`, `adreno_gpu.c:636-653`):

* Allocate a kernel BO of size `fw->size - 4` with flags `MSM_BO_WC | MSM_BO_GPU_READONLY`. There is no `MSM_BO_MAP_PRIV`, so the SQE BO is **not** privileged-mapped. That is consistent with `CP_APRIV_CNTL.ICACHE` (bit 0) being left clear on A650.
* The BO is mapped in the global kernel VM (`gpu->vm`, TTBR1 half, §7).
* **The first dword of the file is skipped**: `memcpy(ptr, &fw->data[4], fw->size - 4)`. Linux does not interpret that header dword.
* Alignment: page granular (the VA allocator is page-granular; `msm_gem*.c` is not in this tree). No stronger alignment is requested.

**Pointing the CP at it.** `CP_SQE_INSTR_BASE` (0x00830/0x00831, phys 0x03d020c0/0x03d020c4) = the BO iova, which corresponds to file offset 4 (`a6xx_gpu.c:1322`). Then `CP_SQE_CNTL = 1` (`:1360`).

**When it is loaded** (`a6xx_ucode_load`, `a6xx_gpu.c:954-1012`). The SQE BO is created once and kept across power cycles. The version check runs immediately after creation; on failure the BO is freed and `-EPERM` is returned, so the GPU is not brought up.

**Version check** (`a6xx_ucode_check_version`, `a6xx_gpu.c:887-952`):

* `buf` is the **BO mapping**, so `buf[0]` is the **file's dword 1** (offset 4).
* For `sqe_name == "a650_sqe.fw"`, the check passes iff `(buf[0] & 0xfff) >= 0x095` (`:934-942`). The source comment says "a650 tier targets don't need whereami but still need to be equal to or newer than 0.95 for other security fixes" (`:908-910`).
* **No feature is enabled by the A650 check.** `has_whereami` stays false; it is only set for a630 (`:924-927`).
* The RPTR-shadow mechanism on A650 instead comes from `hw_apriv` (APRIV hardware RPTR writeback).

**Other buffers created in `ucode_load` for A650:**

| Buffer | Size | Flags | Purpose | Source |
|---|---|---|---|---|
| `shadow` | `4*nr_rings` = 4 bytes | `MSM_BO_WC \| MSM_BO_MAP_PRIV` | RPTR shadow; created because `hw_apriv` | `a6xx_gpu.c:987-999` |
| `pwrup_reglist` | `PAGE_SIZE` | `MSM_BO_WC \| MSM_BO_MAP_PRIV` | Allocated unconditionally but **only used on A7xx** (`a7xx_cp_init`, `a7xx_patch_pwrup_reglist`). XNU can omit it. | `:1001-1009` |

---

## 3. Ringbuffer

**Size and placement.**

* Size: `MSM_GPU_RINGBUFFER_SZ` = **32 KiB** = 8192 dwords, a power of two (`msm_gpu.h:301`, `msm_ringbuffer.c:79-80`).
* The BO is created with `check_apriv(gpu, MSM_BO_WC | MSM_BO_GPU_READONLY)`, which on A650 is `MSM_BO_MAP_PRIV | MSM_BO_WC | MSM_BO_GPU_READONLY` (`msm_ringbuffer.c:91-93`, `msm_gpu.h:767-768`). It is privileged, GPU read-only and CPU write-combined.

**CP_RB_BASE.** `CP_RB_BASE` (0x00800/0x00801) = ring iova (`a6xx_gpu.c:1325`).

**CP_RB_CNTL encoding** (`adreno_common.xml.h`, fields from `adreno_common.xml`):

* `BUFSZ[5:0]` = `ilog2(RB_SZ/8)` = `ilog2(4096)` = 12.
* `BLKSZ[13:8]` = `ilog2(BLKSIZE/8)` = `ilog2(32/8)` = 2 (`msm_gpu.h:302-306`).
* `BUF_SWAP[17:16]`, `POLL_EN` b20, `NO_UPDATE` b27, `RPTR_WR_EN` b31: not set.
* **Linux writes `0x0000020c`** because `hw_apriv` is true (`a6xx_gpu.c:1331-1332`).
* Non-APRIV parts would write `0x0800020c` (with `NO_UPDATE`).

**RPTR shadow.**

* `CP_RB_RPTR_ADDR` (0x00804/0x00805) = `shadow_iova` (ring 0) (`a6xx_gpu.c:1338-1340`).
* The CP writes its read pointer (dword index) there. This is a privileged write (APRIV `RBRPWB`).
* The driver reads it via `a6xx_get_rptr` → `shadow[ring->id]` (`a6xx_gpu.c:2293-2302`).
* `memptrs->rptr` is **not used** on A650; it is only zeroed in `adreno_hw_init`.
* `CP_WHERE_AM_I` is **not** emitted on A650, because `hw_apriv` is true (`a6xx_gpu.c:56-67`).
* The shadow is zeroed by the CPU after programming (`:1341-1342`) and on suspend (`:2132-2134`).

**WPTR semantics** (`a6xx_flush`, `a6xx_gpu.c:69-97`):

1. `ring->cur = ring->next`.
2. `wptr = (cur - start) % 8192`. This is a **dword index**, not a byte offset (`adreno_gpu.h:681-684`).
3. `gpu_write(CP_RB_WPTR [0x00807, phys 0x03d0201c], wptr)` if this is the current ring and not mid-preemption. On A650 both conditions always hold.

* Linux's `a6xx_flush` has no explicit `mb()`. It relies on `writel()` ordering (arm64 `writel` = `wmb` + str). The generic `adreno_flush` does `mb()` (`adreno_gpu.c:739-742`). **XNU: issue a DSB before the WPTR store.**
* Packet emission wraps per dword: `OUT_RING` resets `next` to `start` at `end` (`msm_ringbuffer.h:128-138`). A packet may straddle the wrap point.
* Free space = `(rptr + 8191 - wptr) % 8192`, where `wptr` is computed from `next`. The emitter spins until `cnt+1` dwords are free (`adreno_gpu.c:1081-1097`, `adreno_gpu.h:657-673`).

**memptrs** (`struct msm_rbmemptrs`, `msm_ringbuffer.h:30-40`). One per ring, contiguous in a single BO of `sizeof * nr_rings`, flags `check_apriv(MSM_BO_WC)` = `MAP_PRIV | WC` (`msm_gpu.c:1070-1102`). Layout (compiled):

| Field | Offset | Size | Writer / use on A650 |
|---|---|---|---|
| `rptr` | 0x000 | 4 | unused on A650 (shadow BO used instead) |
| `fence` | 0x004 | 4 | **CP writes the seqno** via CACHE_FLUSH_TS (`rbmemptr(ring, fence)`) |
| `bv_rptr` | 0x008 | 4 | A7xx only |
| `bv_fence` | 0x00c | 4 | A7xx only. Set by sw in `adreno_hw_init`. |
| `stats[64]` | 0x010 | 64×32 | Per submit (`seqno % 64`): `cpcycles_start` +0, `cpcycles_end` +8, `alwayson_start` +16, `alwayson_end` +24 (u64 each) |
| `ttbr0` | 0x810 | 8 | CP_MEM_WRITE in the pagetable switch |
| `context_idr` | 0x818 | 4 | CP_MEM_WRITE (ctx seqno) |
| sizeof | 0x820 (2080) | | |

Address macros: `rbmemptr(ring, m) = memptrs_iova + offsetof(m)` and `rbmemptr_stats(ring, i, m) = rbmemptr(stats) + i*32 + offsetof(m)` (`msm_ringbuffer.h:13-19`).

**Idle detection** (`a6xx_idle`, `a6xx_gpu.c:19-54`):

1. Spin (1 s, `ADRENO_IDLE_TIMEOUT`) until `shadow_rptr == wptr` (`adreno_gpu.c:745-759`, `adreno_gpu.h:242-254`).
2. Spin until all of the following hold (`a6xx_gmu.c:1143-1156`):
   * G:`GPU_GMU_AO_GPU_CX_BUSY_STATUS` (G 0x0930c, phys 0x03d8ec30) bit 23 `GPUBUSYIGNAHB` == 0
   * `RBBM_STATUS` (0x00210, phys 0x03d00840) `& ~CP_AHB_BUSY_CX_MASTER(bit0)` == 0
   * `RBBM_INT_0_STATUS` (0x00201) bit 23 `RBBM_HANG_DETECT` == 0

---

## 4. CP init and secure-mode exit

### 4.1 `a6xx_cp_init()` (`a6xx_gpu.c:803-830`)

Emitted on ring 0 starting at dword 0 (the ring was just reset). This is 9 dwords:

| dw | Value | Meaning |
|---|---|---|
| hdr | `0x70c80008` | PKT7(CP_ME_INIT=0x48, cnt=8) |
| 1 | `0x0000002f` | Field-present mask: bits 0,1,2,3,5. Linux has no comment for this dword. The downstream-KGSL names (not in tree) are: b0 multiple HW contexts, b1 error detection, b2 header dump, b3 default reset state, b5 ucode workarounds. Compare the A7xx mask, documented at `:845-858`: b0 multi-ctx, b1 err detect, b3 reset state. |
| 2 | `0x00000003` | "Enable multiple hardware contexts" (`:811-812`) |
| 3 | `0x20000000` | "Enable error detection" (`:814-815`) |
| 4 | `0x00000000` | "Don't enable header dump" (`:817-819`) |
| 5 | `0x00000000` | (header dump, 2nd dword) |
| 6 | `0x00000000` | "No workarounds enabled" (`:821-822`) |
| 7 | `0x00000000` | pad (`:824-826`) |
| 8 | `0x00000000` | pad |

Then `a6xx_flush` writes **`CP_RB_WPTR = 9`**, and `a6xx_idle` must succeed; otherwise `-EINVAL` (`:828-829`).

### 4.2 Zap shader and secure mode (`a6xx_gpu.c:1371-1399`, `adreno_gpu.c:30-192`)

The GPU comes out of reset in secure mode. It must be switched to non-secure **after CP_ME_INIT has idled, before releasing the GMU OOB**. This happens on every `hw_init`.

**Zap present (DT has `zap-shader` node, SCM available).**

The zap image is loaded only once per boot: `a6xx_zap_shader_init` has `static bool loaded` (`a6xx_gpu.c:1014-1026`). PAS id is `GPU_PAS_ID = 13` (`:17`). Loading steps:

* Firmware name comes from the DT `firmware-name` (apollo: `postmarketos/a650_zap.mbn`). It is loaded with `request_firmware_direct` (`adreno_gpu.c:82-87`).
* The image is loaded with `qcom_mdt_load()` into the reserved region `gpu_mem` = phys **0x8681a000, size 0x2000** (`sm8250.dtsi:818-821`), mapped WC (`adreno_gpu.c:111-151`).
* `qcom_mdt_load` performs the SCM `PAS_INIT_IMAGE` (metadata) and `PAS_MEM_SETUP` calls (`drivers/soc/qcom/mdt_loader.c:285,294`).
* Then `qcom_scm_pas_auth_and_reset(13)`: SMC `x0=0x42000205` (svc PIL 0x02, cmd 0x05), `x1=0x1`, `x2=13` (`qcom_scm.c:706-732`, `qcom_scm.h:101-104`).
* An `-EOPNOTSUPP` from SCM marks zap unavailable for future attempts (`adreno_gpu.c:162-163`).

On every `hw_init` (`a6xx_gpu.c:1380-1385`), once the zap shader has been loaded:

| dw | Value |
|---|---|
| hdr | `0x70e60001` (PKT7 CP_SET_SECURE_MODE=0x66, cnt 1) |
| 1 | `0x00000000` (leave secure mode) |

Then flush (`WPTR = 11`) and `a6xx_idle`. On failure, return `-EINVAL`.

**Zap absent (`-ENODEV`, DT has no available `zap-shader` node).**

* Write `RBBM_SECVID_TRUST_CNTL` (0x0f400, phys 0x03d3d000) = **0** directly from the CPU, with a warning (`a6xx_gpu.c:1386-1396`).
* If the platform's TrustZone protects this register, "a permissions violation will soon follow" (`:1374-1377`).

**Other errors.** Any other error fails `hw_init`. Examples: `-EINVAL` when not built for ARCH_QCOM or when there is no `memory-region`; `-EPROBE_DEFER` when SCM is absent (`adreno_gpu.c:43-46,54-59,186-189`).

**XNU implication.** apollo is a production SM8250 device and its DT has a zap node. The XNU driver must implement the three PAS SMC calls and the `CP_SET_SECURE_MODE` packet. The register write is only a fallback for unlocked platforms.

---

## 5. Submission (`a6xx_submit`, `a6xx_gpu.c:222-305`)

### 5.1 PM4 header encodings (`adreno_gpu.h:642-673`, `adreno_pm4.xml.h`)

`PM4_PARITY(v) = (0x9669 >> (0xF & (v ^ v>>4 ^ v>>8 ^ v>>12 ^ v>>16 ^ v>>20 ^ v>>24 ^ v>>28))) & 1`. This is odd parity: the result is 1 iff `v` has an even number of set bits.

**Type-4 (register write):**

```
PKT4(reg, cnt) = 0x40000000 | (cnt & 0x7f)            /* cnt 1..127, TYPE4_MAX_PAYLOAD */
               | PM4_PARITY(cnt) << 7
               | (reg & 0x3FFFF) << 8
               | PM4_PARITY(reg) << 27
```

It is followed by `cnt` values written to consecutive registers starting at `reg`.

**Type-7 (opcode):**

```
PKT7(op, cnt)  = 0x70000000 | (cnt & 0x7fff)
               | PM4_PARITY(cnt) << 15
               | (op & 0x7F) << 16
               | PM4_PARITY(op) << 23
```

**Precomputed headers** (verified by compiling the Linux macros):

| Packet | Opcode | cnt | Header |
|---|---|---|---|
| CP_ME_INIT | 0x48 | 8 | 0x70c80008 |
| CP_SET_SECURE_MODE | 0x66 | 1 | 0x70e60001 |
| CP_INDIRECT_BUFFER (= CP_INDIRECT_BUFFER_PFE, same opcode; `adreno_pm4.xml:265,387`) | 0x3f | 3 | 0x70bf8003 |
| CP_EVENT_WRITE | 0x46 | 1 | 0x70460001 |
| CP_EVENT_WRITE | 0x46 | 4 | 0x70460004 |
| CP_SMMU_TABLE_UPDATE | 0x53 | 4 | 0x70d30004 |
| CP_MEM_WRITE | 0x3d | 5 | 0x703d8005 |
| CP_WAIT_REG_MEM | 0x3c | 6 | 0x70bc8006 |
| CP_SET_PROTECTED_MODE | 0x5f | 1 | 0x70df0001 |
| CP_REG_TO_MEM | 0x3e | 3 | 0x703e8003 |
| CP_WHERE_AM_I (unused on A650) | 0x62 | 2 | 0x70620002 |
| CP_NOP | 0x10 | 0 | 0x70108000 |
| PKT4 CP_SCRATCH_REG(2) | reg 0x885 | 1 | 0x48088501 |
| PKT4 RBBM_PERFCTR_SRAM_INIT_CMD | reg 0x50e | 1 | 0x40050e01 |

### 5.2 Per-submit ring contents (A650, in order)

The sequence number (`seqno`) is `submit->hw_fence->seqno`: a per-ring, monotonically increasing u32 (`msm_gpu.c:887`). `idx = seqno % 64`. All fields of type `M(x)` are memptrs addresses from §3 (ring 0).

1. **[conditional] Pagetable switch.** Emitted only when `ctx->seqno != ring->cur_ctx_seqno` and the context's VM is a per-process pagetable (§5.3). After the submit, `ring->cur_ctx_seqno = ctx->seqno` (`msm_gpu.c:911`).
2. `CP_REG_TO_MEM`: `0x703e8003`, `0x40080400`, `lo(M(stats[idx].cpcycles_start))`, `hi(...)`. Dword 0 = `REG(RBBM_PERFCTR_CP(0)=0x400) | CNT(2)<<18 | 64B(bit30)` (`a6xx_gpu.c:99-108,234-235`).
3. `CP_REG_TO_MEM`: `0x703e8003`, `0x40080980` (CP_ALWAYS_ON_COUNTER 0x980, 2 dwords, 64B), `lo/hi(M(stats[idx].alwayson_start))` (`:242-243`).
4. `CP_EVENT_WRITE`: `0x70460001`, `0x00000018` (PC_CCU_INVALIDATE_DEPTH=24) (`:246-247`).
5. `CP_EVENT_WRITE`: `0x70460001`, `0x00000019` (PC_CCU_INVALIDATE_COLOR=25) (`:249-250`).
6. For each user cmd (`:253-279`):
   * `MSM_SUBMIT_CMD_BUF`: `0x70bf8003`, `lo(iova)`, `hi(iova)`, `size_dwords & 0xFFFFF` (`A5XX_CP_INDIRECT_BUFFER_2_IB_SIZE`, bits 19:0).
   * `CTX_RESTORE_BUF`: emitted the same way, only if the context changed.
   * `IB_TARGET_BUF`: skipped.
   * The size is in dwords and the iova is the BO iova + `offset*4`. Userspace size must be a multiple of 4 bytes (`msm_gem_submit.c:223-245,696-703`).
   * Every 32 IBs Linux calls `update_shadow_rptr`, which is a no-op on A650.
7. `CP_REG_TO_MEM` cpcycles_end; `CP_REG_TO_MEM` alwayson_end (same encodings as steps 2-3) (`:281-284`).
8. `PKT4 CP_SCRATCH_REG(2)`: `0x48088501`, `seqno`. This is for debugging only; `recover` prints it (`:287-288`).
9. **Fence:** `CP_EVENT_WRITE`: `0x70460004`, `0x80000004` (EVENT=CACHE_FLUSH_TS(4) | IRQ(bit31)), `lo(M(fence))`, `hi(M(fence))`, `seqno` (`:294-299`). CACHE_FLUSH_TS flushes UCHE and then writes the 32-bit `seqno` to `memptrs+0x4` (`adreno_pm4.xml:13-17`). The IRQ bit raises `RBBM_INT_0 CP_CACHE_FLUSH_TS` (bit 20) afterwards. RB_DONE_TS is **not** used on A6xx; it appears only in `A7XX_INT_MASK`.
10. `a6xx_flush` → `CP_RB_WPTR = new dword index`.

**Size estimate.** A submit with no context switch and N IBs costs `4+4+2+2 + 4N + 4+4 + 2 + 5` = **27 + 4N** dwords. A context switch adds 26 dwords.

**Scheduler limits.** Linux allows at most 8 jobs in flight per ring (`num_hw_submissions`, `msm_ringbuffer.c:10-12`). The ring holds 8192 dwords.

### 5.3 `a6xx_set_pagetable` for A650 (`a6xx_gpu.c:110-220`)

**When it runs.** It returns early if `ctx->seqno == ring->cur_ctx_seqno` (`:121-122`). It also returns early if `msm_iommu_pagetable_params` fails (`:124-125`). That happens when the context uses the **global** VM: there is no per-process pagetable, so nothing is emitted.

**XNU option.** A single shared address space needs no `CP_SMMU_TABLE_UPDATE` at all.

**Inputs:**

* `ttbr` = the physical address of the context's ARM LPAE stage-1 level-0/1 table (`ttbr0_cfg.arm_lpae_s1_cfg.ttbr`, with no ASID bits) (`msm_iommu.c:611`).
* `asid` = **0** for every pagetable, the same as the TTBR1 ASID (`msm_iommu.c:613-619`).
* `sysprof` = `refcount(sysprof_active) > 1`. It is false by default (`msm_gpu.c:1106`).

**Packets (A650, sysprof false):**

| # | Packet | Dwords |
|---|---|---|
| 1 | CP_SET_PROTECTED_MODE(0) | `0x70df0001`, `0x00000000` (`:150-155`) |
| 2 | PKT4 RBBM_PERFCTR_SRAM_INIT_CMD = 1 | `0x40050e01`, `0x00000001` (`:157-158`) |
| 3 | CP_SMMU_TABLE_UPDATE | `0x70d30004`, `lo32(ttbr)`, `(hi32(ttbr) & 0xffff) \| (asid << 16)` = `hi32(ttbr)`, `0` (CONTEXTIDR, unused for aarch64), `0` (CONTEXTBANK = 0) (`:162-169`, `adreno_pm4.xml:2223-2240`) |
| 4 | CP_MEM_WRITE | `0x703d8005`, `lo(M(ttbr0))`, `hi(M(ttbr0))`, `lo32(ttbr)`, `hi32(ttbr)`, `ctx->seqno`. This writes `memptrs.ttbr0` (u64) and `memptrs.context_idr`, for debug and preemption (`:175-180`). |
| 5 | CP_EVENT_WRITE | `0x70460001`, `0x00000031` (CACHE_INVALIDATE=49), to invalidate UCHE (`:197-198`) |
| 6 | CP_WAIT_REG_MEM | `0x70bc8006`, `0x00000003` (FUNCTION=WRITE_EQ), `0x0000050f` (POLL_ADDR_LO = RBBM_PERFCTR_SRAM_INIT_STATUS register offset), `0` (POLL_ADDR_HI), `1` (REF), `1` (MASK), `0` (DELAY_LOOP_CYCLES) (`:205-212`) |
| 7 | CP_SET_PROTECTED_MODE(1) | `0x70df0001`, `0x00000001` (`:214-218`) |

This totals 26 dwords. The A7xx-only CP_WAIT_TIMESTAMP, CP_THREAD_CONTROL and CP_RESET_CONTEXT_STATE are **not** emitted on A650 (`:127-148,187-190`).

**What the SMMU and CP need for this to work:**

* **The GPU must use SMMU context bank 0.** Linux forces CB0 for SID 0 (`arm-smmu-qcom.c:268-286`). This is because the packet's CONTEXTBANK is 0 and `adreno_hw_init` grants the CP access to CB0 via SCM call `0x42000C1B` (§1.4).
* **Split TTBR.** The CB0 stage-1 format must be AArch64 LPAE with `IO_PGTABLE_QUIRK_ARM_TTBR1` (`arm-smmu-qcom.c:339-342`):
  * Kernel/global VM goes in TTBR1 (upper half; GPU VA bit 48 set).
  * Per-process VMs go in TTBR0.
  * When the first per-process pagetable is created, `set_ttbr0_cfg` clears `TCR.EPD0/EPD1` and installs TTBR0 with the CB's ASID (`arm-smmu-qcom.c:227-266`, `msm_iommu.c:575-605`). The TTBR0 config is a clone of the TTBR1 config minus the TTBR1 quirk (`msm_iommu.c:526-531`).
* **CPU-side TLB maintenance.** Linux does **no** CPU-side SMMU TLB invalidate for the switch; the CP firmware performs the CB0 TTBR0 update. Any TLB invalidation inside `CP_SMMU_TABLE_UPDATE` is firmware behavior and is not visible in the Linux source. Pagetable modifications made by the CPU are flushed through the io-pgtable TLB ops (`msm_iommu.c:609`).
* **No GPU register holds a "gpu-offset" or SMMU base.** The only "aperture" concept in upstream is the SCM `CP_SMMU_APERTURE` call (svc `QCOM_SCM_SVC_MP`=0x0c, cmd 0x1b, arg0 = `0xffff0000 | cb`).

### 5.4 Preemption

* A650 runs with **1 ring**, so preemption is effectively off.
  * `a6xx_preempt_init` returns early for `nr_rings <= 1` (`a6xx_preempt.c:418-419`).
  * `a6xx_preempt_hw_init` returns early (`:194-196`).
  * `a6xx_preempt_trigger` returns early (`:233-234`).
  * `a7xx_preempt_start` returns early (`a6xx_gpu.c:781-782`).
* No `CP_CONTEXT_SWITCH_*`, `CP_SET_PSEUDO_REG`, `CP_SET_MARKER` or `CP_CONTEXT_SWITCH_YIELD` packets are emitted, and no `CP_WHERE_AM_I` (APRIV).
* **To keep preemption disabled:** use one ring and never write `CP_CONTEXT_SWITCH_CNTL` (0x008a0). Do not port the `enable_preemption=1` behavior (§0).
* `MSM_PARAM_PRIORITIES` reports `1 * NR_SCHED_PRIORITIES` = 3 software priority levels on the single ring.

---

## 6. Interrupts, fences, hang detection

**Mask.** `RBBM_INT_0_MASK` (0x00038, phys 0x03d000e0) = `A6XX_INT_MASK` = **0x03d0e242** (`a6xx_gpu.c:1028-1038`):

| Bit | Name | Handling in `a6xx_irq` |
|---|---|---|
| 1 | CP_AHB_ERROR | log |
| 6 | RBBM_ATB_ASYNCFIFO_OVERFLOW | log |
| 9 | CP_HW_ERROR | `a6xx_cp_hw_err_irq`: read CP_INTERRUPT_STATUS (0x00823) (`:1652-1692`) |
| 13 | CP_IB2 | (no handler, just cleared) |
| 14 | CP_IB1 | (no handler) |
| 15 | CP_RB (alias PM4CPINTERRUPT) | (no handler) |
| 20 | **CP_CACHE_FLUSH_TS** | **retire**: `msm_gpu_retire()` (`:1785-1788`) |
| 22 | RBBM_ATB_BUS_OVERFLOW | log |
| 23 | RBBM_HANG_DETECT | `a6xx_fault_detect_irq` → recovery (`:1694-1731`) |
| 24 | UCHE_OOB_ACCESS | log |
| 25 | UCHE_TRAP_INTR | (no handler) |

Bit definitions are in `a6xx.xml:29-66` (bitset `A6XX_RBBM_INT_0_MASK`) and `a6xx.xml:459`.

**IRQ handler** (`a6xx_gpu.c:1754-1794`):

* `status = R RBBM_INT_0_STATUS` (0x00201, phys 0x03d00804).
* `W RBBM_INT_CLEAR_CMD` (0x00037, phys 0x03d000dc) = `status`. The ack happens first, before dispatch.
* Then dispatch per the table above.

**CP_HW_ERROR decode** (CP_INTERRUPT_STATUS bits):

| Bit | Meaning | Extra read |
|---|---|---|
| 0 | OPCODE_ERROR | `W CP_SQE_STAT_ADDR(0x908)=1`, then `R CP_SQE_STAT_DATA(0x909)` |
| 1 | UCODE_ERROR | |
| 2 | HW_FAULT | `R CP_HW_FAULT(0x821)` |
| 4 | REGISTER_PROTECTION_ERROR | `R CP_PROTECT_STATUS(0x824)`: bit20 = read, `[17:0]` = register |
| 5 | AHB_ERROR | |
| 6 | VSD_PARITY | |
| 7 | ILLEGAL_INSTR | |

**Fence completion.**

* Completion is determined **only** from the memstore value `memptrs->fence` (`msm_gpu.c:864-873`), compared wrap-safe against submitted seqnos.
* The IRQ is only a wakeup. The hangcheck timer also calls `msm_gpu_retire` as a "workaround for missing irq" (`msm_gpu.c:668-669`).
* `CP_SCRATCH_REG(2)` (0x00885) also holds the last seqno the CP **parsed**, not completed. Linux does not use it for completion.

**Hang detection.**

* **Hardware.** `RBBM_INTERFACE_HANG_INT_CNTL` = `0x401fffff`: bit 30 enables it and the low bits are the timeout. This raises RBBM_HANG_DETECT (bit 23). The handler ignores it when `RBBM_STATUS3` (0x00213) bit 24 `SMMU_STALLED_ON_FAULT` is set (`a6xx_gpu.c:1706-1707`).
* **Software.** The hangcheck timer period is 500/2 = **250 ms**, halved because `progress()` exists (`msm_gpu.c:1004-1014`). It is re-armed on each submit (`:914`). Each time it fires:
  1. If `memptrs->fence` changed, the GPU made progress.
  2. Otherwise, if work is pending, call `a6xx_progress`. That function compares `CP_IB1_BASE` (0x928/9), `CP_IB2_BASE` (0x92b/c), `CP_IB1_REM_SIZE` (0x92a) + `CP_ROQ_AVAIL_IB1` (0x949) `>>16`, and `CP_IB2_REM_SIZE` (0x92d) + `CP_ROQ_AVAIL_IB2` (0x94a) `>>16` against the previous sample (`a6xx_gpu.c:2304-2335`). Up to 3 such retries are allowed.
  3. If there is still no progress, schedule recovery (`msm_gpu.c:638-670`).
* **Recovery** (`a6xx_recover`, `a6xx_gpu.c:1442-1516`):
  * `CP_SQE_CNTL = 3` (halt SQE).
  * Force a CX GDSC power-collapse and then a full `hw_init`.
  * Skip the hung submit by forcing `memptrs->fence = hung seqno` (`msm_gpu.c:534-543`).
  * Replay the remaining submits.
* **Idle bits.** `RBBM_STATUS` (0x00210) busy bits, from bit 23 down: GPU_BUSY_IGN_AHB(23), GPU_BUSY_IGN_AHB_CP(22), HLSQ(21), VSC(20), TPL1(19), SP(18), UCHE(17), VPC(16), VFD(15), TESS(14), PC_VSD(13), PC_DCALL(12), COM_DCOM(11), LRZ(10), A2D(9), CCU(8), RB(7), RAS(6), TSE(5), VBIF(4), GFX_DBGC(3), CP_BUSY(2), CP_AHB_BUSY_CP_MASTER(1), CP_AHB_BUSY_CX_MASTER(0) (`a6xx.xml:319-346`). Idle means `(RBBM_STATUS & ~1) == 0`.

---

## 7. GPU memory constraints

**Two address-space layers** (`adreno_gpu.c:201-262`, `a6xx_gpu.c:2261-2291`, `msm_iommu.c`, `arm-smmu.c:828-839`):

* **Global/kernel VM (`gpu->vm`).**
  * CB0 TTBR1 with `IO_PGTABLE_QUIRK_ARM_TTBR1`. The domain aperture is `[~0 << ias, ~0]`, where `ias = SMMU va_size` (from IDR2.UBS, normally 48).
  * Linux uses `start = max(SZ_16M, aperture_start) & GENMASK_ULL(48,0)` and `size = aperture_end - aperture_start + 1`.
  * With ias = 48, kernel GPU VAs are **`0x0001_0000_0000_0000`–`0x0001_FFFF_FFFF_FFFF`**: 49-bit addresses with bit 48 = 1, which selects TTBR1.
  * When mapping, msm sign-extends bit 48 into bits 63:49 for the SMMU API (`msm_iommu.c:679-681`).
  * The GPU registers/packets receive the 49-bit form. Example: `CP_RB_BASE_HI` = `0x0001xxxx`.
  * `uche_trap_base` `0x1_FFFF_FFFF_F000` sits at the top of this range.
* **Per-process VM.** TTBR0, start `ADRENO_VM_START` = **`0x1_0000_0000`** (4 GiB, to catch missing `_HI` writes), size `BIT(ias) - 4 GiB` = **`0x0000_FFFF_0000_0000`** for ias = 48 (`adreno_gpu.h:550-551`, `adreno_gpu.c:238-262`). The `address_space_size` module parameter overrides it. There is no 4 GB VA quirk on A650.
* **A650 GMEM** is not mapped into the UCHE range (no `UCHE_GMEM_RANGE_*` programming), and `MSM_PARAM_GMEM_BASE` = 0.
* **HTW quirk.** `IO_PGTABLE_QUIRK_ARM_OUTER_WBWA` is only added if an HTW LLCC slice exists **and** the SMMU is not cache-coherent. The SM8250 GPU SMMU is `dma-coherent`, so it is not added (`a6xx_gpu.c:2268-2276`, `sm8250.dtsi:3109`).

**Alignment.**

* VA allocation is page-granular (4 KiB). Linux requests no larger alignment for the ring, SQE or memptrs.
* IBs must be dword aligned, with size and offset in dwords (`msm_gem_submit.c:223-245`).
* The fence address (memptrs+4) is 4-byte aligned. Stats addresses are 8-byte aligned, and must be for 64-bit `CP_REG_TO_MEM`.

**Privilege and caching.**

* `MSM_BO_MAP_PRIV` → `IOMMU_PRIV`. It leaves `ARM_LPAE_PTE_AP_UNPRIV` (PTE bit 6) clear, so only **privileged** GPU masters can access the page (`io-pgtable-arm.c:490-494`).
* `MSM_BO_GPU_READONLY` → AP RDONLY (bit 7).
* On A650, `CP_APRIV_CNTL` = 0x6e makes the following accesses privileged: ringbuffer fetch, RPTR-shadow writeback, and memory accesses made by PM4 packets *in the ringbuffer* (RBPRIVLEVEL). That last group covers the fence `CP_EVENT_WRITE`, the stats `CP_REG_TO_MEM` and the memptrs `CP_MEM_WRITE` (`a6xx.xml:102-117`). Packets inside user IBs run unprivileged, so they cannot touch PRIV-mapped buffers. Consequence: kernel packets that write memptrs must be placed in the ring, not in an IB.
* All kernel BOs are `MSM_BO_WC` (CPU write-combine / uncached).
* `MSM_BO_CACHED_COHERENT` (`IOMMU_CACHE`, I/O coherent through the coherent SMMU) is allowed for userspace BOs because `has_cached_coherent` = true (`adreno_device.c:228-229`). The enforcement lives in `msm_gem.c`, which is not in this tree.

**Minimal kernel-owned buffer set:**

| Buffer | Size | Flags (A650) | Registers / uses |
|---|---|---|---|
| ringbuffer | 32 KiB | PRIV, WC, GPU_RO | CP_RB_BASE |
| memptrs | 2080 B (1 ring) → 1 page | PRIV, WC | fence target (+0x4), stats, ttbr0 |
| RPTR shadow | 4 B → 1 page | PRIV, WC | CP_RB_RPTR_ADDR |
| SQE ucode | `fw_size - 4` | WC, GPU_RO (not PRIV) | CP_SQE_INSTR_BASE |
| zap image | ≤ 0x2000 | carve-out phys 0x8681a000 (not GPU-mapped by Linux; handed to TZ) | SCM PAS 13 |
| pwrup_reglist | 4 KiB | PRIV, WC | allocated but unused on A650 (optional) |
| GMU buffers (HFI, logs, ...) | — | — | outside this spec (`a6xx_gmu.c`) |

---

## 8. GET_PARAM values reported for A650 (`adreno_get_param`, `adreno_gpu.c:367-458`; IDs from `include/uapi/drm/msm_drm.h:74-95`)

| Param (id) | A650 value | Notes |
|---|---|---|
| MSM_PARAM_GPU_ID (0x01) | **650** | `info->revn` |
| MSM_PARAM_GMEM_SIZE (0x02) | **0x120000** (1 MiB + 128 KiB) | |
| MSM_PARAM_CHIP_ID (0x03) | **0x06050002** | Speedbin bits are not OR'ed in because `revn != 0` (`:393-397`). |
| MSM_PARAM_MAX_FREQ (0x04) | `fast_rate` = the highest OPP enabled for the speed bin | OPP `supported-hw` masks (`sm8250.dtsi:2988-3029`) with `supp_hw = BIT(speedbin)` (`a6xx_gpu.c:2337-2382`): fuse 0 → 587 MHz; fuse 1 → 670 MHz; fuse 2 (bin 3) → 670 MHz; fuse 3 (bin 2) → 525 MHz. With no nvmem fuse, the OPP filter is not applied, so 670 MHz. |
| MSM_PARAM_TIMESTAMP (0x05) | 64-bit `CP_ALWAYS_ON_COUNTER` (0x00980 LO, 0x00981 HI; phys 0x03d02600), 19.2 MHz ticks | Read with GMU OOB PERFCOUNTER held: set bit 28, wait for ack bit 30, clear with bit 29 (non-legacy) (`a6xx_gpu.c:2177-2194`, `a6xx_gmu.c:312-320`). Read LO first; it latches HI (`msm_gpu.h:642-650`). |
| MSM_PARAM_GMEM_BASE (0x06) | **0** | a650 family (`:386-392`) |
| MSM_PARAM_PRIORITIES (0x07) | **3** | `nr_rings(1) * NR_SCHED_PRIORITIES`. `NR_SCHED_PRIORITIES = 1 + DRM_SCHED_PRIORITY_LOW - DRM_SCHED_PRIORITY_HIGH` (`msm_gpu.h:340`) = 3 with current DRM enum values. |
| MSM_PARAM_PP_PGTABLE (0x08) | 0 | deprecated |
| MSM_PARAM_FAULTS / SUSPENDS (0x09/0x0a) | counters | |
| MSM_PARAM_VA_START (0x0e) | **0x1_0000_0000** | Requires a per-process VM, otherwise EINVAL (`:427-431`). |
| MSM_PARAM_VA_SIZE (0x0f) | **0xFFFF_0000_0000** (ias = 48) | Same requirement. |
| MSM_PARAM_HIGHEST_BANK_BIT (0x10) | **16** | |
| MSM_PARAM_RAYTRACING (0x11) | 0 | |
| MSM_PARAM_UBWC_SWIZZLE (0x12) | **6** | forced (`a6xx_gpu.c:620`) |
| MSM_PARAM_MACROTILE_MODE (0x13) | **1** | from the sm8250 UBWC table; verify (§0.1) |
| MSM_PARAM_UCHE_TRAP_BASE (0x14) | **0x1_FFFF_FFFF_F000** | |
| MSM_PARAM_HAS_PRR (0x15) | **0** | `sm8250-smmu-500` is excluded from PRR (`arm-smmu-qcom.c:357-359`) |

There is no RBBM-perfcounter-based timestamp on A650. `RBBM_PERFCTR_CP(0)` (0x00400) is only sampled into memptrs stats as CP cycles. `CP_PERFCTR_CP_SEL(0) = 0` (ALWAYS_COUNT) makes it count every CP cycle.

---

## 9. Risks and open points for the XNU port

1. **GMU dependency.** Every step above assumes the GMU firmware (`a650_gmu.bin`) is booted, the GX GDSC is on and the OOB handshake works. `hw_init` starts and ends with GMU OOB GPU_SET (bits 30/31). Without the GMU, CX/GX power and `CX_BUSY` idle detection do not work. This needs its own spec (`a6xx_gmu.c`).
2. **Secure mode.** On the production apollo device, `RBBM_SECVID_TRUST_CNTL` is most likely TZ-protected. XNU must implement the PAS SMC sequence (init image, mem setup, auth-and-reset id 13) and use `CP_SET_SECURE_MODE 0`. The SCM CP-SMMU-aperture call (`0x42000C1B`) is also needed before any `CP_SMMU_TABLE_UPDATE`.
3. **UBWC SoC table is not in the tree.** `RBBM_NC_MODE_CNTL=1` and `MACROTILE_MODE=1` assume upstream `sm8250_data.macrotile_mode = true`. RB/TPL1/SP/UCHE values depend only on hbb 16, swizzle 6 and UBWC 4.0.
4. **TTBR1 / 49-bit VA.** Kernel BO addresses have bit 48 set. If XNU uses a single TTBR0 domain instead, every iova written to CP registers must still lie within the configured T0SZ range. Do not reuse Linux's `0x0001_xxxx_xxxx_xxxx` values without the TTBR1 split.
5. **SQE version offset.** The version is read from file dword **1**, not dword 0, because dword 0 is stripped before the check. The SQE BO must not be PRIV-mapped, since APRIV ICACHE is off.
6. **CP_PROTECT(47).** The XML says the array length is 32, but the A650 last-span entry must go to `0x0087f`.
7. **Re-init on every power-up.** Linux redoes the entire sequence, including ring reset to dword 0, after each runtime suspend (66 ms autosuspend). The first submit after init always re-emits the pagetable switch.
8. **Barriers.** The ring and memptrs are WC. Issue a DSB before the `CP_RB_WPTR` write, and read the fence and RPTR shadow as volatile uncached data.
