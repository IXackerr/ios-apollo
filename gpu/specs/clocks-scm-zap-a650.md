# Adreno 650 (SM8250 / Xiaomi Mi 10T Pro "apollo") — Clocks, GDSCs, SCM and Zap-Shader Spec

Target: a from-scratch XNU driver that pokes registers directly (no Linux clock framework, no genpd, no regulator framework).

Source tree for every `file:line` citation: `apollo-linux` (royka1/apollo-linux, branch apollo-6.17) (abbreviated as below), plus the stock Android DT `kona-v21.dts (decompiled stock DTB)`.

| Abbrev | Path |
|---|---|
| `gpucc` | drivers/clk/qcom/gpucc-sm8250.c |
| `gcc` | drivers/clk/qcom/gcc-sm8250.c |
| `pll` | drivers/clk/qcom/clk-alpha-pll.c |
| `pll.h` | drivers/clk/qcom/clk-alpha-pll.h |
| `rcg2` | drivers/clk/qcom/clk-rcg2.c |
| `br` / `br.h` | drivers/clk/qcom/clk-branch.c / clk-branch.h |
| `gdsc` / `gdsc.h` | drivers/clk/qcom/gdsc.c / gdsc.h |
| `dtsi` | arch/arm64/boot/dts/qcom/sm8250.dtsi |
| `apollo.dts` | arch/arm64/boot/dts/qcom/sm8250-xiaomi-apollo.dts |
| `scm` / `scm.h` | drivers/firmware/qcom/qcom_scm.c / qcom_scm.h |
| `smc` | drivers/firmware/qcom/qcom_scm-smc.c |
| `mdt` / `mdt.h` | drivers/soc/qcom/mdt_loader.c / include/linux/soc/qcom/mdt_loader.h |
| `adreno` | drivers/gpu/drm/msm/adreno/adreno_gpu.c |
| `a6xx` | drivers/gpu/drm/msm/adreno/a6xx_gpu.c |
| `gmu` | drivers/gpu/drm/msm/adreno/a6xx_gmu.c |
| `stock` | kona-v21.dts (decompiled stock DTB) |

Facts marked **[ext]** come from upstream files that are *not* present in this tree (`clk-rcg.h`, `include/linux/arm-smccc.h`, `arch/arm64/kernel/smccc-call.S`). They are standard upstream definitions, but you cannot check them against this checkout.

---

## 0. Base addresses

| Block | Phys base | Size | Source |
|---|---|---|---|
| GCC | `0x0010_0000` | `0x1f0000` | dtsi:949-951 |
| GPU_CC | `0x03D9_0000` | `0x9000` (max reg `0x8008`) | dtsi:3073-3076, gpucc:284 |
| GPU (kgsl_3d0) | `0x03D0_0000` | `0x40000` | dtsi:2961-2966 |
| GMU | `0x03D6_A000` | `0x30000` | dtsi:3033-3041 |
| GPU RSCC | `0x03DE_0000` | `0x10000` | dtsi:3038 |
| GMU PDC / PDC seq | `0x0B29_0000` / `0x0B49_0000` | `0x10000` each | dtsi:3039-3040 |
| Adreno SMMU | `0x03DA_0000` | `0x10000` | dtsi:3087-3091 |
| Zap reserved region (`gpu_mem`) | `0x8681_A000` | `0x2000` (8 KiB), `no-map` | dtsi:818-821; stock:24588-24593 |
| cmd-db (RPMh command DB) | `0x8086_0000` | `0x20000`, `no-map` | dtsi:782-786 |

All register addresses below are absolute physical addresses: GCC = `0x100000 + off`, GPU_CC = `0x3D90000 + off`.

---

## A. Clocks and power

### A.0 Generic register semantics (applies to every clock below)

**Branch clock (CBCR) — `clk_branch2_ops`.**
- **Enable:** read-modify-write `enable_reg |= enable_mask` (`clk_enable_regmap`). Then wait as dictated by `halt_check` (br:94-109, br:62-92).
- **CBCR bits:** bit0 `CLK_ENABLE`, bits[7:4] `SLEEP`, bits[11:8] `WAKEUP`, bit12/13/14 `FORCE_MEM_*`, bits[30:28] `NOC_FSM_STATUS`, bit31 `CLK_OFF` (br.h:58-67).
- **"Is on" test (`clk_branch2_check_halt`, enabling):** `(CBCR & BIT31) == 0` **or** `((CBCR >> 28) & 7) == 2` (`FSM_STATUS_ON`) (br:43-60). The disable test is `CBCR & BIT31` set.
- **halt_check types** (br.h:29-35, br:62-92):
  - `BRANCH_HALT` (0): after enable, poll the "is on" test up to 200 × `udelay(1)`, then fail with `-EBUSY`.
  - `BRANCH_HALT_VOTED` (0x80): on enable, same poll as HALT. On disable, `udelay(10)` only.
  - `BRANCH_VOTED` (0x80 alone, used by `gpu_cc_hlos1_vote_gpu_smmu_clk`): behaves like HALT_VOTED.
  - `BRANCH_HALT_DELAY` (2): no status check, just `udelay(10)`.
- If the halt register is the vote register (GCC `0x152000`), there is no CBCR status bit to poll. Those clocks are all HALT_DELAY.

**RCG2 root (`clk_rcg2_ops`).**
- **Layout:** `CMD_RCGR` at `cmd_rcgr+0x0`: bit0 `UPDATE`, bit1 `ROOT_EN`, bit31 `ROOT_OFF` (status). `CFG` at `+0x4`: bits[4:0] `SRC_DIV` (hid_width bits), bits[10:8] `SRC_SEL`, bits[13:12] `MODE`, bit20 `HW_CLK_CTRL`. M/N/D are at `+0x8/+0xC/+0x10` (rcg2:26-50).
- **Divider encoding:** `F(f, src, h, m, n)` stores `pre_div = 2*h - 1` **[ext: clk-rcg.h]**. So h=1 → 1, h=1.5 → 2, h=2 → 3.
- **Configure** (`clk_rcg2_configure`, rcg2:580-598):
  1. Read CFG.
  2. Clear `SRC_SEL`, `SRC_DIV(hid_width)`, `MODE` and `HW_CLK_CTRL`. Set `SRC_SEL = parent_map cfg` and `SRC_DIV = pre_div`. Set `MODE = dual-edge (2<<12)` only if mnd_width && n && m != n (rcg2:503-562).
  3. Write CFG.
  4. `update_config`: set `CMD.UPDATE` (bit0), then poll until bit0 reads 0, 500 × `udelay(1)`. On timeout, fail with `-EBUSY` (rcg2:114-136).
- **Enable:** RCG2 ops have no enable op. The root is enabled by hardware when a downstream branch is enabled. `CMD.ROOT_OFF` (bit31) == 0 means the root is running (rcg2:65-76). This is a useful debug check after you enable a child CBCR.

**Alpha PLL MODE bits** (pll:16-34): bit0 `OUTCTRL`, bit1 `BYPASSNL`, bit2 `RESET_N`, bit20 `VOTE_FSM_ENA`, bit22 `UPDATE`, bit23 `UPDATE_BYPASS`, bit29 `ACK_LATCH`, bit30 `ACTIVE_FLAG`, bit31 `LOCK_DET`.
- `wait_for_pll()` polls MODE up to 1500 × `udelay(1)`, then fails with `-ETIMEDOUT` (pll:383-410).
- `OPMODE` values: `STANDBY = 0`, `RUN = 1` (pll:335-337). `PLL_OUT_MASK = 0x7` covers USER_CTL bits[2:0] (pll:337).

### A.1 GCC side (base `0x00100000`)

| Clock (DT ID) | Enable reg (abs) | Bit | Halt reg / check | Who uses it | Source |
|---|---|---|---|---|---|
| GPLL0 (FSM-voted PLL) | `0x152018` (APCS GPLL vote) | bit0 | MODE `0x100000` bit30 `ACTIVE_FLAG` poll (fixed-lucid = trion-fixed enable, FSM branch) | parent of everything | gcc:35-50, pll:1080-1097, pll.h:181 |
| gpll0_out_even (post-div /2) | USER_CTL `0x10000C` bits[11:8] (value `0x1` = ÷2); Linux never writes it | — | — | parent of gpu_gpll0_div | gcc:52-72 |
| `GCC_GPU_GPLL0_CLK_SRC` (34) | `0x152000` | **bit15** | none (`BRANCH_HALT_DELAY` → 10 µs) | gpucc parent 5 (GPLL0 main) | gcc:1387-1402 |
| `GCC_GPU_GPLL0_DIV_CLK_SRC` (35) | `0x152000` | **bit16** | none (`BRANCH_HALT_DELAY` → 10 µs) | gpucc parent 6 (GPLL0 even) → **GMU 200 MHz** | gcc:1404-1419 |
| `GCC_GPU_CFG_AHB_CLK` (33) | `0x171004` | bit0 | not modelled as a clock; **Linux forces it ON at GCC probe and never turns it off** | GPU CC / GPU config AHB | gcc:3651 |
| `GCC_GPU_MEMNOC_GFX_CLK` (37) | `0x17100C` | bit0 | `0x17100C` `BRANCH_HALT_VOTED` | GMU "memnoc", SMMU "bus" | gcc:1434-1445; dtsi:3048,3105 |
| `GCC_GPU_SNOC_DVM_GFX_CLK` (38) | `0x171018` | bit0 | `0x171018` `BRANCH_HALT` | SMMU "iface" | gcc:1447-1458; dtsi:3106 |
| `GCC_DDRSS_GPU_AXI_CLK` (21) | `0x171154` | bit0 | `0x171154` `BRANCH_HALT_VOTED` | GMU "axi" | gcc:1268-1279; dtsi:3047 |
| `GCC_GPU_IREF_EN` (36) | `0x18C014` | bit0 | `0x18C014` `BRANCH_HALT` | **not referenced** by mainline GPU/GMU/SMMU nodes | gcc:1421-1432 |
| `GCC_GPU_BCR` (reset) | `0x171000` | bit0 = assert | — | not used by msm driver | gcc:3536 |
| GPU GPLL0 "MISC" gate | `0x171028` | bits[1:0] = `0b11` at GCC probe ("Disable the GPLL0 active input to … GPU via MISC registers") | — | makes GPLL0 reach GPU_CC **only** through vote bits 15/16 above | gcc:3639-3644 |

DT IDs come from `include/dt-bindings/clock/qcom,gcc-sm8250.h:31,43-48`.

**Left on by the bootloader?** Not derivable from source for any of these clocks, so treat all of them as "unknown". Points to note:
- Linux itself forces `GCC_GPU_CFG_AHB_CLK` on (gcc:3651) and sets MISC `0x171028[1:0]=3` (gcc:3644). That suggests Linux does not trust the boot state.
- GPLL0 is shared SoC-wide and Linux never configures it (it has no `configure` call, only an FSM vote). It is effectively assumed running.
- Its rate is not in source, but the gpucc frequency table implies GPLL0 = 600 MHz and gpll0_out_even = 300 MHz: `F(200000000, P_GPLL0_OUT_MAIN_DIV, 1.5, …)` (gpucc:84). **Verify at runtime:** GPLL0 `L_VAL` is at `0x100004` (Lucid/Trion layout, pll:134-149), and rate = 19.2 MHz × L (+ alpha/2^16).
- **Recommendation:** read each CBCR's bit31 at boot for diagnostics, then unconditionally enable everything listed in A.5.

**GCC parent rule for GMU 200 MHz:** `GPU_CC_CX_GMU_CLK` → `gpu_cc_gmu_clk_src` (src sel 6) → `gcc_gpu_gpll0_div_clk_src` (vote `0x152000` bit16) → `gpll0_out_even` → `gpll0` (vote `0x152018` bit0) (gpucc:68-80, 143-159; gcc:1404-1419, 35-72).

### A.2 GPU_CC (base `0x03D90000`)

#### A.2.1 Register map used

| Name | Offset | Abs addr | Notes | Source |
|---|---|---|---|---|
| GPU_CC_XO_BCR | 0x1000 | 0x03D91000 | reset | gpucc:272 |
| `GPU_CC_CXO_AON_CLK` | 0x1004 | 0x03D91004 | bit0, HALT_VOTED, not used by mainline DT | gpucc:174-185 |
| GPU_CC_GX_BCR | 0x1008 | 0x03D91008 | reset; stock calls it gx "sw-reset" | gpucc:271; stock:3441-3445 |
| **GX GDSCR** | 0x100C | 0x03D9100C | | gpucc:241-250 |
| GX CFG_GDSCR | 0x1010 | 0x03D91010 | `gdscr + 4` | gdsc:34 |
| `GPU_CC_GX_GMU_CLK` | 0x1064 | 0x03D91064 | bit0, HALT; parent = gmu_clk_src. **Not** in the mainline GMU clock list | gpucc:200-216 |
| GPU_CC_CX_BCR | 0x1068 | 0x03D91068 | reset | gpucc:268 |
| **CX GDSCR** | 0x106C | 0x03D9106C | | gpucc:231-239 |
| CX CFG_GDSCR | 0x1070 | 0x03D91070 | not polled for CX | gdsc:66-71 |
| `GPU_CC_AHB_CLK` | 0x1078 | 0x03D91078 | bit0, **HALT_DELAY** (10 µs, no poll) | gpucc:104-115 |
| `GPU_CC_CRC_AHB_CLK` | 0x107C | 0x03D9107C | bit0, HALT_VOTED, unused | gpucc:117-128 |
| `GPU_CC_CX_APB_CLK` | 0x1088 | 0x03D91088 | bit0, HALT_VOTED, unused by mainline | gpucc:130-141 |
| `GPU_CC_CX_SNOC_DVM_CLK` | 0x108C | 0x03D9108C | bit0, HALT_VOTED, unused by mainline | gpucc:161-172 |
| `GPU_CC_CX_GMU_CLK` | 0x1098 | 0x03D91098 | bit0, **HALT** (poll); parent = gmu_clk_src | gpucc:143-159 |
| `GPU_CC_CXO_CLK` | 0x109C | 0x03D9109C | bit0, **HALT** (poll) | gpucc:187-198 |
| GPU_CC_GFX3D_AON_BCR | 0x10A0 | 0x03D910A0 | reset | gpucc:269 |
| GPU_CC_GMU_BCR | 0x111C | 0x03D9111C | reset | gpucc:270 |
| **GMU RCG CMD_RCGR** | 0x1120 | 0x03D91120 | | gpucc:89-102 |
| GMU RCG CFG_RCGR | 0x1124 | 0x03D91124 | | rcg2:35-46 |
| GPU_CC_ACD_BCR | 0x1160 | 0x03D91160 | reset | gpucc:267 |
| GX clamp_io_ctrl | 0x1508 | 0x03D91508 | bit0 = clamp, bit4 = AON reset; stock calls it gx "domain-addr" | gpucc:243; gdsc:27-28; stock:3435-3439 |
| CX gds_hw_ctrl | 0x1540 | 0x03D91540 | bit31 `PWR_ON` status for CX; stock "hw-ctrl-addr" | gpucc:233; gdsc:20,66-91; stock:3415 |
| `GPU_CC_HLOS1_VOTE_GPU_SMMU_CLK` | 0x5000 | 0x03D95000 | bit0, BRANCH_VOTED, **not referenced** in mainline DT | gpucc:218-229 |
| GPU_CC_PLL1 | 0x0100 | 0x03D90100.. | Lucid (= Trion register layout) | gpucc:51-66; pll.h:23 |

Reset (BCR) semantics: write bit0 = 1 to assert, 0 to deassert. This is the standard `qcom_reset_map` behaviour; reset.c is not in the tree. Linux's msm driver does not touch these BCRs for a650.

#### A.2.2 GPU_CC_PLL1 (Lucid) — configuration values

`CLK_ALPHA_PLL_TYPE_LUCID == CLK_ALPHA_PLL_TYPE_TRION` (pll.h:22-23) and `clk_lucid_pll_configure == clk_trion_pll_configure` (pll.h:221-222).

**Trion/Lucid register offsets** (pll:134-149), with absolute addresses for PLL1 (base `0x03D90100`):

| Reg | Off | Abs |
|---|---|---|
| MODE | 0x00 | 0x03D90100 |
| L_VAL | 0x04 | 0x03D90104 |
| CAL_L_VAL | 0x08 | 0x03D90108 |
| USER_CTL | 0x0C | 0x03D9010C |
| USER_CTL_U | 0x10 | 0x03D90110 |
| USER_CTL_U1 | 0x14 | 0x03D90114 |
| CONFIG_CTL | 0x18 | 0x03D90118 |
| CONFIG_CTL_U | 0x1C | 0x03D9011C |
| CONFIG_CTL_U1 | 0x20 | 0x03D90120 |
| TEST_CTL | 0x24 | 0x03D90124 |
| TEST_CTL_U | 0x28 | 0x03D90128 |
| TEST_CTL_U1 | 0x2C | 0x03D9012C |
| STATUS | 0x30 | 0x03D90130 |
| OPMODE | 0x38 | 0x03D90138 |
| ALPHA_VAL | 0x40 | 0x03D90140 |

**`gpu_cc_pll1_config`** (gpucc:40-49):

| Field | Value |
|---|---|
| l | `0x1a` (26) |
| alpha | `0xaaa` |
| config_ctl_val | `0x20485699` |
| config_ctl_hi_val | `0x00002261` |
| config_ctl_hi1_val | `0x029a699c` |
| user_ctl_val | `0x00000000` |
| user_ctl_hi_val | `0x00000805` |
| user_ctl_hi1_val | `0x00000000` |
| test_ctl_* | all 0 (not set) |

- **Alpha width:** Trion has no `ALPHA_VAL_U`, so `pll_alpha_width()` gives 16 bits (pll:371-373).
- **Rate:** `19.2 MHz × (26 + 0xaaa/65536)` = 499.2 + 0.7998 ≈ **500.0 MHz** (pll:659-663).
- **VCO table:** 249.6 MHz – 2000 MHz (gpucc:36-38).
- **Who uses PLL1:** PLL1 feeds only the 500 MHz GMU entry (src sel 3) (gpucc:85). **At 200 MHz (what Linux uses, gmu:1079) PLL1 is not needed.** Linux only *configures* it at probe and never enables it for sm8250 GMU. You may skip PLL1 entirely.

#### A.2.3 `clk_trion_pll_configure` — exact write order (pll:1696-1741)

`clk_alpha_pll_write_config()` **skips the write when the value is 0** (pll:436-441).

0. **Skip-if-running check** (`trion_pll_is_enabled`, pll:1059-1071). Read MODE (`0x03D90100`) and OPMODE (`0x03D90138`). If `(OPMODE & 1) && (MODE & 1)`, the PLL is already running: **return without writing anything** (pll:1703-1706).
1. `L_VAL 0x03D90104 = 0x0000001A`
2. `CAL_L_VAL 0x03D90108 = 0x00000044`. This one is unconditional `regmap_write`; `TRION_PLL_CAL_VAL = 0x44` (pll:341, 1709).
3. `ALPHA_VAL 0x03D90140 = 0x00000AAA`
4. `CONFIG_CTL 0x03D90118 = 0x20485699`
5. `CONFIG_CTL_U 0x03D9011C = 0x00002261`
6. `CONFIG_CTL_U1 0x03D90120 = 0x029A699C`
7. `USER_CTL 0x03D9010C`: **not written** (value 0)
8. `USER_CTL_U 0x03D90110 = 0x00000805`
9. `USER_CTL_U1 0x03D90114`: **not written** (0)
10. `TEST_CTL`, `TEST_CTL_U`, `TEST_CTL_U1`: **not written** (0)
11. `MODE |= BIT(23)` (`UPDATE_BYPASS`)
12. `MODE &= ~BIT(0)` (`OUTCTRL` off)
13. `OPMODE 0x03D90138 = 0` (STANDBY)
14. `MODE |= BIT(2)` (`RESET_N`): the PLL is now in standby, out of reset

#### A.2.4 Lucid enable path (only if you want GMU at 500 MHz)

Clock ops: `clk_alpha_pll_lucid_ops` = prepare `alpha_pll_lucid_prepare`, enable/disable `clk_trion_pll_enable/disable` (pll:1841-1850).

**prepare** (`__alpha_pll_trion_prepare(hw, LUCID_PCAL_DONE)`, pll:1748-1775):
- Read STATUS `0x03D90130`. If bit27 (`LUCID_PCAL_DONE`, pll:345) is set, calibration is already done: skip.
- Otherwise run **enable** and then **disable** once to calibrate.

**enable** (`clk_trion_pll_enable`, pll:1080-1115):
1. Read MODE. If bit20 `VOTE_FSM_ENA` is set, the PLL is FSM-voted. Write the vote (PLL1 has no `enable_reg`, so this is a no-op), then poll MODE bit30 `ACTIVE_FLAG` (1500 µs). Done.
2. Otherwise:
   1. `OPMODE 0x03D90138 = 1` (RUN).
   2. Poll MODE bit31 `LOCK_DET`, up to 1500 × 1 µs (pll:383-416). On timeout, `-ETIMEDOUT`.
   3. `USER_CTL 0x03D9010C |= 0x7` (main/aux/aux2 outputs).
   4. `MODE |= BIT(0)` (OUTCTRL).

**disable** (pll:1117-1148): `MODE &= ~1` → `USER_CTL &= ~7` → `OPMODE = 0` → `MODE |= BIT(2)`.

#### A.2.5 GMU RCG `gpu_cc_gmu_clk_src`

- `cmd_rcgr = 0x1120` → CMD `0x03D91120`, CFG `0x03D91124`. `mnd_width = 0` (no M/N/D registers used), `hid_width = 5` (gpucc:89-102).
- **Parent map** (gpucc:68-73):

  | CFG SRC_SEL | Source |
  |---|---|
  | 0 | BI_TCXO 19.2 MHz |
  | 3 | GPU_CC_PLL1 |
  | 5 | GCC gpu_gpll0_clk_src (GPLL0 main) |
  | 6 | GCC gpu_gpll0_div_clk_src (GPLL0 even) |

- **Frequency table** (gpucc:82-87):

  | Rate | Source | h | pre_div | CFG value |
  |---|---|---|---|---|
  | 19.2 MHz | TCXO | 1 | 1 | `0x001` |
  | **200 MHz** | GPLL0_DIV | 1.5 | **2** | **`0x602`** |
  | 500 MHz | PLL1 | 1 | 1 | `0x301` |

**Program 200 MHz** (the rate Linux sets, gmu:1079):
1. Make sure the source is running: GCC `0x152000 |= BIT(16)` (and GPLL0 voted via `0x152018` bit0), then wait 10 µs. See risk R3.
2. `v = read(0x03D91124)`, then `v &= ~(0x1F | (7<<8) | (3<<12) | BIT(20))`, then `v |= (6<<8) | 2`. Write `v` back. Expect `0x00000602` if the other bits were 0.
3. `write(0x03D91120, read(0x03D91120) | 1)` (UPDATE).
4. Poll `0x03D91120` bit0 == 0, 500 × 1 µs. On timeout, error.
5. After enabling `GPU_CC_CX_GMU_CLK`, `0x03D91120` bit31 (`ROOT_OFF`) should read 0.

**CX_GMU CBCR wake/sleep tuning done at gpucc probe** (gpucc:23-26, 315-322): read-modify-write `0x03D91098` with mask `0x00000FF0`, value `0x00000FF0` (WAKEUP[11:8] = 0xF, SLEEP[7:4] = 0xF). Do this once, before enabling the clock.

#### A.2.6 Branch clocks needed

| Consumer (DT) | Clock | Abs CBCR | Enable | Wait |
|---|---|---|---|---|
| GMU "ahb", SMMU "ahb" | GPU_CC_AHB_CLK | 0x03D91078 | bit0=1 | 10 µs delay only (HALT_DELAY) |
| GMU "gmu" | GPU_CC_CX_GMU_CLK | 0x03D91098 | bit0=1 | poll bit31==0 or [30:28]==2, ≤200 µs |
| GMU "cxo" | GPU_CC_CXO_CLK | 0x03D9109C | bit0=1 | poll as above, ≤200 µs |

Consumer bindings: GMU clocks at dtsi:3044-3049 (order ahb, gmu, cxo, axi, memnoc). SMMU clocks at dtsi:3104-3107 (ahb, bus = GCC_GPU_MEMNOC_GFX, iface = GCC_GPU_SNOC_DVM_GFX).

**Not needed by mainline:** `GX_GMU`, `CX_APB`, `CX_SNOC_DVM`, `CRC_AHB`, `CXO_AON`, `HLOS1_VOTE_GPU_SMMU`. There is **no `GPU_CC_HUB_*` clock on sm8250**: `gmu->hub_clk` is looked up by name "hub" and comes back NULL here (gmu:1749-1750, gmu:1080-1081 is a no-op).

### A.3 GDSCs

**GDSCR bits** (gdsc:20-47): bit0 `SW_COLLAPSE`, bit1 `HW_CONTROL`, bit2 `SW_OVERRIDE`, [15:12] `CLK_DIS_WAIT`, [19:16] `EN_FEW_WAIT`, [23:20] `EN_REST_WAIT`, bit31 `PWR_ON`.
**CFG_GDSCR** (`gdscr+4`): bit15 `POWER_DOWN_COMPLETE`, bit16 `POWER_UP_COMPLETE`.

**Poll timing:** `gdsc_poll_status` busy-polls for up to **2000 µs** (`STATUS_POLL_TIMEOUT_US`), then does one last check, then fails with `-ETIMEDOUT` (gdsc:49,103-117).

**Status register choice** (gdsc:60-94):
- If the flags include `POLL_CFG_GDSCR`, use CFG_GDSCR bit16 (on) / bit15 (off).
- Else if `gds_hw_ctrl` is set, use its bit31.
- Else use GDSCR bit31.

#### GPU_CX_GDSC (gpucc:231-239)
- `gdscr = 0x106C` (abs `0x03D9106C`), `gds_hw_ctrl = 0x1540` (abs `0x03D91540`), `pwrsts = OFF_ON`, `flags = VOTABLE`.
- No clamp, no supply, no parent genpd (mainline gpucc node has no `power-domains`, dtsi:3073-3085).
- Stock difference: `clk-dis-wait-val = 8` and `gds-timeout = 500` µs; the parent supply is `cx.lvl` (stock:3421-3433).

#### GPU_GX_GDSC (gpucc:241-250)
- `gdscr = 0x100C` (abs `0x03D9100C`), `clamp_io_ctrl = 0x1508` (abs `0x03D91508`).
- `flags = CLAMP_IO | AON_RESET | POLL_CFG_GDSCR`, `pwrsts = OFF_ON`.
- **`pd.power_on = gdsc_gx_do_nothing_enable`**: Linux never powers GX on from the CPU (gdsc:635-667).
- Stock: parent supply `gfx.lvl` (pm8150a_s1_level), `sw-reset` = `0x3D91008`, `domain-addr` = `0x3D91508`, `qcom,reset-aon-logic` (stock:3447-3459, 4276-4287).

#### One-time init (`gdsc_init`, gdsc:403-508), applied to **both** GDSCRs at gpucc probe
1. Read-modify-write GDSCR with mask `0x00FFF006` (HW_CONTROL | SW_OVERRIDE | the three wait fields) and value `0x00282000` (EN_REST=2, EN_FEW=8, CLK_DIS=2) (gdsc:413-427, 37-39). For CX you may use CLK_DIS=8 to match stock.
2. If already on (by the status register rule above) and VOTABLE (CX only): clear SW_COLLAPSE so the APSS vote exists (gdsc:454-458).

#### CX enable — what the CPU driver must do (`gdsc_enable`, gdsc:259-315, with `gdsc_toggle_logic`, gdsc:139-187)
1. (No SW_RESET, no CLAMP_IO for CX.)
2. `0x03D9106C &= ~BIT(0)` (clear SW_COLLAPSE = vote ON).
3. `udelay(1)` (needed because `gds_hw_ctrl` is set; gdsc:163-175).
4. Poll `0x03D91540` bit31 == 1 for ≤ 2000 µs (stock uses 500 µs). On timeout, fail.
5. `force_mem_on`: no-op (no `cxcs`).
6. `udelay(1)` (gdsc:286-293).
7. No HW_CTRL and no RETAIN_FF.

#### CX disable (`gdsc_disable`, gdsc:317-364)
`0x03D9106C |= BIT(0)`. VOTABLE and not synced, so **no status poll**: just `udelay(500)` (gdsc:150-161). The domain may stay on because of other voters (the GMU).

#### GX disable (Linux runs it on every `a6xx_gmu_stop` via `pm_runtime_put_sync(gxpd)`, gmu:1245-1251)
1. `0x03D9100C |= BIT(0)`.
2. Poll `0x03D91010` bit15 (`POWER_DOWN_COMPLETE`) ≤ 2000 µs.
3. Assert clamp: `0x03D91508 |= BIT(0)` (gdsc:231-241, 360-361).

This must happen **before** the CX clocks and the CX GDSC are turned off (gmu:1245-1255, gdsc:635-647).

#### GX "full" enable (for reference only — Linux never runs it)
1. AON reset: `regmap_update_bits(0x1508, BIT(4), 1)`, then `udelay(1)`, then `regmap_update_bits(0x1508, BIT(4), 0)` (gdsc:243-250).
   **Note the Linux bug:** `val = 1` masked with `BIT(4)` writes 0, so the reset pulse is a no-op upstream.
2. Clear clamp `0x03D91508 &= ~1`.
3. Clear SW_COLLAPSE.
4. Poll CFG `0x03D91010` bit16.

#### Ownership
- **CPU owns GPU_CX_GDSC.** `gmu->cxpd` is attached and device-linked with `DL_FLAG_PM_RUNTIME` (gmu:2043-2053), so `pm_runtime_get(gmu->dev)` powers CX.
- **The GMU firmware owns GX power-up.** The CPU only holds a dummy reference (gmu:1070-1076) and forces GX off at stop (gmu:1245-1251).
- Also note that on resume Linux does **not** de-clamp or un-collapse GX (the dummy enable does nothing), even though its stop path set SW_COLLAPSE and the clamp. The GMU firmware therefore handles those bits on its own power-up. See risk R5.

### A.4 Rails / RPMh prerequisites
- **CX and MX:** mainline sm8250 attaches **no RPMh power-domain** to gpucc, the GMU or the GPU. The gpucc node has no `power-domains` (dtsi:3073-3085), and the GMU node lists only the gpucc GDSCs (dtsi:3051-3053). Linux makes **no CPU-side CX/MX vote** for the GPU and relies on CX/MX being on while the APSS runs.
  - Stock does vote: gpucc has `vdd_cx-supply` (cx.lvl, pm8150_s3_level) and `vdd_mx-supply` (mx.lvl, pm8150a_s3_level) (stock:3159-3168, 3833-3844, 3866-3880).
  - The GMU OPP is 200 MHz at `RPMH_REGULATOR_LEVEL_MIN_SVS` (dtsi:3062-3069). That level is used only to build the `cx_arc_votes` table sent to the GMU.
- **GX (gfx.lvl):** voted **by the GMU firmware** through the GPU RSC/PDC. The CPU builds the vote tables:
  - `gx_arc_votes` from cmd-db aux data `"gfx.lvl"` with secondary `"gmxc.lvl"`, falling back to `"mx.lvl"`. Index per GPU OPP level (dtsi:2986-3030 opp-level).
  - `cx_arc_votes` from `"cx.lvl"`.
  - Vote word = `(pri_level<<16) | (sec_index<<8) | pri_index` (gmu:1452-1561).
  - These tables reach the GMU via HFI. **Your driver must parse cmd-db** (reserved `0x80860000`, dtsi:782-786) to build them.
- **RSC handshake** around GMU boot/stop: `a6xx_rpmh_start/stop` (gmu:516-562) and `a6xx_gmu_rpmh_init`, which programs the PDC/RSCC (gmu:566-).
- **DDR bandwidth:** the a650 catalog entry has no `bcms` (a6xx_catalog.c:965-988), and the mainline GPU node has no `interconnects`. So Linux sends **no DDR bandwidth vote** for the GPU on sm8250, and `a6xx_gmu_set_initial_bw` is effectively a no-op. Stock uses `qcom,gpu-bus-table` msm-bus votes (stock:23787-23800). See risk R6.

### A.5 Recommended ordered bring-up (mirrors `a6xx_gmu_resume`, gmu:1055-1141)

**One-time (gpucc / GCC probe equivalent):**
1. GCC MISC: `0x171028 |= 0x3` (gcc:3644).
2. GCC: `0x171004 |= 1` (GPU_CFG_AHB, gcc:3651).
3. Optional: PLL1 configure (A.2.3).
4. `0x03D91098`: read-modify-write `[11:4] = 0xFF` (gpucc:319-322).
5. GDSC init for CX (`0x03D9106C`) and GX (`0x03D9100C`) (A.3).

**Every power-up:**
1. CX GDSC on (A.3) — `pm_runtime_get_sync(gmu->dev)` (gmu:1068).
2. GPLL0 vote `0x152018 |= 1` and poll `0x100000` bit30. Then `0x152000 |= BIT(16)` (GPLL0 div → GPU) and `|= BIT(15)` (optional), and wait 10 µs.
3. GMU RCG = 200 MHz: CFG `0x03D91124` ← src 6 / div 2, then UPDATE (A.2.5) (gmu:1079).
4. Enable in DT/bulk order (gmu:1082; dtsi:3044-3049):
   1. `0x03D91078` (AHB, 10 µs)
   2. `0x03D91098` (CX_GMU, poll)
   3. `0x03D9109C` (CXO, poll)
   4. `0x171154` (DDRSS_GPU_AXI, poll)
   5. `0x17100C` (MEMNOC_GFX, poll)
5. SMMU additionally needs `0x171018` (SNOC_DVM_GFX, poll) (dtsi:3104-3109).
6. Then the GMU firmware boot / RSC start / HFI (out of scope here). The GMU brings GX up.

**Power-down** (gmu:1225-1258): GMU slumber/force-off → GX disable sequence (A.3) → disable the clocks above → CX GDSC collapse vote.

---

## B. SCM / TrustZone calls (ARMv8 SMCCC, owner SIP)

### B.1 Encoding
- **Function ID** [ext SMCCC]: `ARM_SMCCC_CALL_VAL(type, cc, owner, fn) = (type<<31) | (cc<<30) | ((owner&0x3F)<<24) | (fn&0xFFFF)`. `OWNER_SIP = 2`. type: `STD (yielding) = 0`, `FAST = 1`. cc: `SMC_64 = 1`, `SMC_32 = 0`.
- **QCOM function number:** `SCM_SMC_FNID(svc, cmd) = ((svc & 0xFF) << 8) | (cmd & 0xFF)` (scm.h:72). x0 is built at smc:165-169.
- **Call type:** `qcom_scm_call()` (non-atomic, used for every GPU/PAS call) → **STD**. `qcom_scm_call_atomic()` → **FAST** (smc:159).
- So for the ARM_64 convention: **x0 = `0x42000000 | FNID`** (STD), `0xC2000000 | FNID` (FAST). For ARM_32: `0x02000000|FNID` (STD) / `0x82000000|FNID` (FAST).
- **x1 = arginfo** `QCOM_SCM_ARGS(n, t0..t9)` = `n & 0xF | (t0<<4) | (t1<<6) | … | (t9<<22)`. Types: `VAL = 0`, `RO = 1`, `RW = 2`, `BUFVAL = 3` (scm.h:22-42).
- **x2..x5 = args[0..3]** (smc:170-172).
- **More than 4 args** (arglen = arginfo & 0xF > 4): x2..x4 = args[0..2]. **x5 = physical address of a buffer holding args[3..9]** (7 entries, LE64 for ARM_64 or LE32 for ARM_32), allocated from the TZ-mem pool (smc:32-36, 174-201). **None of the GPU calls need this.**
- x6 = quirk session value (0 initially); x7 = 0 (smc:38-55).
- **Return values:** x0 = status; results = x1, x2, x3 (smc:207-213).
  - Status 0 = success.
  - Error remap (scm.h:159-185): `-1` → `-EIO`, `-2`/`-3` → `-EINVAL`, `-4` → `-EOPNOTSUPP`, `-5` → `-ENOMEM`, `-12` → `-EBUSY`, any other nonzero → `-EINVAL`.
  - The PAS wrappers return `ret ?: res.result[0]`, so a **nonzero x1 with x0 == 0 is also treated as failure** (passed up as a positive value) (scm:637, 695, 732).

### B.2 Interrupted / busy / waitq handling (smc:38-148)
1. **INTERRUPTED** (`x0 == 1`) (smc:46-54): re-issue the SMC with **x0 = 1** (the returned value is used as the new function ID) and x1–x5 unchanged. **x6 = the x6 value returned by the previous SMC** (`ARM_SMCCC_QUIRK_QCOM_A6`: smccc-call.S saves x6 into `quirk.state.a6` [ext]). Loop while `x0 == 1`.
2. **WAITQ_SLEEP** (`x0 == 2`) (smc:93-118):
   1. `wq_ctx = x1` (must be 0, scm:2106-2118) and `smc_call_ctx = x2`.
   2. Block until the SCM IRQ fires. The IRQ handler calls `GET_WQ_CTX` (FAST, `x0 = 0xC2002403`, no args; returns x1 = wq_ctx, x2 = flags (must be `WAKE_ONE = 1`), x3 = more_pending) and completes the wait (scm:2146-2171, smc:70-91, scm:125).
   3. Then issue `WAITQ_RESUME`: STD `x0 = 0x42002402`, `x1 = 0x1`, `x2 = smc_call_ctx` (smc:57-68). Loop.
   - **sm8250's scm node has no interrupt** (dtsi:672-676), so Linux would block forever on WAITQ_SLEEP. In practice this TZ is not expected to return it. Treat `x0 == 2` as a fatal error.
3. **EBUSY** (`x0 == -12`): sleep 30 ms and retry, at most 22 attempts in total. After that the result is `-EBUSY` (smc:29-30, 130-145).
4. Calls are serialized by a global mutex (smc:27, 131-135). Do the same, with one SMC in flight across all CPUs.

### B.3 Convention detection (`__get_convention`, scm:226-291)
1. ARM_64 probe: **FAST** `IS_CALL_AVAIL`.
   - `x0 = 0xC2000601`
   - `x1 = 0x1`
   - `x2 = 0x02000601` (FNID(6,1) | SIP<<24)

   If x0 == 0 and **x1 == 1**, the convention is ARM_64.
2. Else ARM_32 probe: `x0 = 0x82000601` with the same args. On success, ARM_32.
3. Else LEGACY. Not expected on sm8250, and legacy uses a different, buffer-based ABI (`SCM_LEGACY_FNID = svc<<10 | cmd`, scm.h:79). That ABI is not covered here.

### B.4 Calls needed for the GPU (ARM_64 STD encodings)

| Call | svc/cmd | x0 | x1 (arginfo) | x2.. | Result | Source |
|---|---|---|---|---|---|---|
| `is_call_available(svc,cmd)` | 0x06/0x01 | `0x42000601` | `0x1` | x2 = `0x02000000 \| (svc<<8) \| cmd`, e.g. PIL init `0x02000201`, auth `0x02000205` | x1 != 0 → available | scm:343-372; scm.h:113-114 |
| PAS `is_supported(13)` | 0x02/0x07 | `0x42000207` | `0x1` | x2 = 13 | x1 != 0 → supported. Linux first checks `is_call_available(2,7)` | scm:779-798; scm.h:106 |
| PAS `init_image(13, mdata)` | 0x02/0x01 | `0x42000201` | **`0x82`** = ARGS(2, VAL, RW) | x2 = 13, x3 = **phys of metadata buffer** | ok if x0 == 0 && x1 == 0 | scm:578-638; scm.h:102 |
| PAS `mem_setup(13, addr, size)` | 0x02/0x02 | `0x42000202` | `0x3` | x2 = 13, x3 = `0x8681A000`, x4 = (max_addr − min_addr) | same | scm:667-696; scm.h:103 |
| PAS `auth_and_reset(13)` | 0x02/0x05 | `0x42000205` | `0x1` | x2 = 13 | same | scm:706-733; scm.h:104 |
| PAS `shutdown(13)` (not used by msm) | 0x02/0x06 | `0x42000206` | `0x1` | x2 = 13 | | scm:742-769 |
| `set_remote_state(state, id)` | 0x01/0x0A | `0x4200010A` | `0x2` | x2 = state (`SCM_GPU_ZAP_SHADER_RESUME = 0`), x3 = 13 | x1 | scm:470-487; scm.h:94; a5xx_gpu.c:642 |
| WAITQ_RESUME | 0x24/0x02 | `0x42002402` | `0x1` | x2 = smc_call_ctx | | smc:57-68 |
| GET_WQ_CTX (FAST) | 0x24/0x03 | `0xC2002403` | 0 | — | x1..x3 | smc:70-91 |

**GPU PAS ID = 13 (0xD).** Sources: a6xx:17 (`#define GPU_PAS_ID 13`) and stock `qcom,kgsl-hyp { qcom,pas-id = <0x0d>; qcom,firmware-name = "a650_zap"; }` (stock:23707-23712).

**Metadata buffer for `init_image`** (scm:593-610, 628-636):
- Linux uses `dma_alloc_coherent()` on the SCM device: **physically contiguous, 4 KiB aligned, non-cacheable** (the comment says this avoids XPU violations, because TZ XPU-locks the blob during the call).
- **Do not** place it in an SHM-bridge / TZ-mem pool buffer: the hypervisor creates the bridge itself, and a buffer already bridged makes the call fail (scm:598-604).
- Copy the metadata in (memcpy), then make the SMC.
- With `ctx == NULL` (the zap path passes NULL via `qcom_mdt_load`, mdt:459), the buffer is **freed immediately after the call**, whether it succeeded or failed (scm:629-630).
- In XNU: allocate contiguous pages, map them uncached (or clean+invalidate to PoC after the copy), pass the PA, then free.

**Around each PAS call** Linux enables the optional SCM core/iface/bus clocks and the interconnect vote (scm:149-213, 612-626). The sm8250 scm node has none of these (dtsi:672-676), so there is nothing to do.

`QCOM_SCM_SVC_GPU` / `GPU_INIT_REGS` (`0x42002801`) is used only for a7xx/a750 (a6xx:1945-1961). It is **not** needed for a650.

---

## C. Zap-shader load (A650)

### C.1 Which file
- **Mainline apollo DT:** `zap-shader { memory-region = <&gpu_mem>; firmware-name = "postmarketos/a650_zap.mbn"; }` (apollo.dts:612-619). With `firmware-name` present, Linux does `request_firmware_direct(<that exact path>)` and **does not** try the `qcom/` prefix (adreno:82-87, 140-142).
- **Without `firmware-name`:** the catalog name `"a650_zap.mdt"` (a6xx_catalog.c:976) goes through `adreno_request_fw()`, which tries in order (adreno:527-599):
  1. `qcom/a650_zap.mdt` (direct)
  2. `a650_zap.mdt` (direct)
  3. `qcom/a650_zap.mdt` (usermode helper)

  The segment files are then looked up under the same prefix (adreno:140-151).
- **Split segment names:** strip the last 3 characters of the name and append `bNN`, e.g. `a650_zap.b00` … `a650_zap.b02` (mdt:76-84).
- **Stock Android:** PIL name `"a650_zap"`, so the split form is `a650_zap.mdt` + `a650_zap.bNN` from the vendor firmware partition. The directory is downstream convention and not derivable here.
- The image is **OEM-signed**. Use the phone's own blobs. A `.mbn` is just the `.mdt` and `.bNN` squashed into one ELF.

### C.2 Reserved memory
- `0x8681A000`, size **`0x2000` (8 KiB)**, `no-map`, identical in mainline (dtsi:818-821) and stock (`pil_gpu_region@8681a000`, `removed-dma-pool`, stock:24588-24593).
- In the decompiled stock base DT the region's phandle `0x659` is not referenced by any node: `kgsl-hyp` has no `memory-region` there (stock:23707-23712). The region is still reserved, so use it.
- XNU must exclude it from the VM and must not keep a cacheable alias of it.

### C.3 ELF / MDT rules (mdt.h:7-9)
- `QCOM_MDT_TYPE_MASK = 7<<24`, `QCOM_MDT_TYPE_HASH = 2<<24`, `QCOM_MDT_RELOCATABLE = BIT(27)` (all in `p_flags`).
- **Header validity** (`mdt_header_valid`, mdt:21-52): ELF magic; `e_phentsize == sizeof(Elf32_Phdr)` (32); phdr table inside the file; section-header table, if present, sized right and inside the file. **The zap image is ELF32.**
- **Loadable segment** (mdt:54-66): `p_type == PT_LOAD`, **not** a hash segment, and `p_memsz != 0`.
- **Required size** (`qcom_mdt_get_size`, mdt:109-138): over the loadable phdrs, `min_addr = min(p_paddr)` and `max_addr = ALIGN(max(p_paddr+p_memsz), 4K)`. Size = `max_addr - min_addr`, which must be ≤ `0x2000`, or Linux fails with `-E2BIG` (adreno:117-122).

### C.4 Metadata (`qcom_mdt_read_metadata`, mdt:160-227) — what goes to `init_image`
1. Require `e_phnum >= 2` and `phdr[0].p_type != PT_LOAD`. phdr[0] is the NULL segment covering the ELF header plus the phdr table.
2. `hash_segment` = the first index ≥ 1 whose `(p_flags & 0x07000000) == 0x02000000`. If there is none, `-EINVAL`.
3. Buffer = `phdr[0].p_filesz` bytes copied from file offset 0, **immediately followed** by `hash_size = phdr[hash].p_filesz` bytes of hash.
4. Where the hash comes from:
   - if `ehdr_size + hash_size == fw->size` (a split `.mdt`): from file offset `phdr[0].p_filesz`;
   - else if `phdr[hash].p_offset + hash_size <= fw->size` (`.mbn`): from `p_offset`;
   - else from the split file `.bNN` for N = hash index.
5. Length = `ehdr_size + hash_size`.

### C.5 Full algorithm (`zap_shader_load_mdt` → `qcom_mdt_load` → `auth_and_reset`)
`zap_shader_load_mdt` is at adreno:30-174, `qcom_mdt_load` at mdt:452-465.

1. Find the `zap-shader` node and its `memory-region`, giving `mem_phys = 0x8681A000` (adreno:48-66).
2. Load the firmware file (C.1).
3. `mem_size = qcom_mdt_get_size(fw)` and check it is ≤ the region size (adreno:111-122).
4. Map `[mem_phys, mem_phys+mem_size)` **write-combine** (`memremap(..., MEMREMAP_WC)`, adreno:125).
5. `qcom_mdt_pas_init` (mdt:241-304):
   1. Scan the loadable phdrs. `relocate = any(p_flags & BIT(27))`. Compute `min_addr` / `max_addr` as in C.3.
   2. Read the metadata (C.4), then **SCM `PAS_INIT_IMAGE(13, metadata_pa)`**. The temporary copy is freed (mdt:285-286). Any error stops here.
   3. If `relocate`: **SCM `PAS_MEM_SETUP(13, mem_phys = 0x8681A000, max_addr - min_addr)`** (mdt:293-300).
6. Copy the segments (`__qcom_mdt_load`, mdt:335-437):
   1. `is_split` = some phdr with `p_filesz != 0` lies beyond the end of the file (mdt:307-333).
   2. `mem_reloc = relocate ? min_addr : mem_phys` (mdt:375-387).
   3. For each loadable phdr:
      - `offset = p_paddr - mem_reloc`. Require `0 <= offset` and `offset + p_memsz <= mem_size`, else `-EINVAL`.
      - Require `p_filesz <= p_memsz`.
      - Destination = `mem_region + offset`.
      - Not split: copy `p_filesz` bytes from `fw + p_offset` (bounds-checked).
      - Split: load file `.bNN` (N = phdr index) directly into the destination; its size must equal `p_filesz`.
      - Zero-fill `p_memsz - p_filesz`.
   - **Non-relocatable images must therefore be linked at `p_paddr` within `0x8681A000…+0x2000`.** Relocatable images are placed at `mem_phys + (p_paddr - min_addr)`.
7. **SCM `PAS_AUTH_AND_RESET(13)`** (adreno:155-165).
8. Unmap the region and release the firmware (adreno:167-173).
   - In XNU: if your mapping was cacheable, **clean to PoC before auth**. After auth, TZ XPU-protects the region, so drop the mapping.

**Order:** `INIT_IMAGE` → (`MEM_SETUP` only if relocatable) → copy segments → `AUTH_AND_RESET`.

**When Linux runs it:** inside `hw_init` after `a6xx_cp_init`, with the GMU up and a GPU OOB set (so GX is on) (a6xx:1064-1078, 1367-1378). On success, the CPU then emits `CP_SET_SECURE_MODE 0` on ring 0, flushes, and waits for idle (a6xx:1379-1385).

### C.6 Subsequent power-ups / resume
- **a6xx (incl. a650):** `a6xx_zap_shader_init` uses a static `loaded` flag. After the first success it returns 0 **without any SCM call** (a6xx:1014-1026). Each `hw_init` then just issues `CP_SET_SECURE_MODE 0` (a6xx:1379-1385). There is **no `set_remote_state` on a6xx** in mainline.
- **a5xx only:** resume via `qcom_scm_set_remote_state(SCM_GPU_ZAP_SHADER_RESUME = 0, GPU_PAS_ID = 13)`, i.e. `x0 = 0x4200010A, x1 = 0x2, x2 = 0, x3 = 13` (a5xx_gpu.c:642-678). Keep it only as a fallback experiment for A650.

### C.7 Failure handling (adreno:28-191; a6xx:1378-1399)

| Condition | Linux result | Effect in a6xx `hw_init` |
|---|---|---|
| SCM not probed | `-EPROBE_DEFER` | hw_init fails |
| `zap-shader` node absent / disabled | `-ENODEV`, `zap_available = false` | **fallback:** write `RBBM_SECVID_TRUST_CNTL = 0` directly. Faults if TZ actually locked it |
| `memory-region` missing | `-EINVAL`, `zap_available = false` | hw_init fails (not ENODEV); later calls return `-ENODEV` → fallback |
| firmware missing | `request_firmware` error | hw_init fails |
| bad ELF / size | `-EINVAL` / `-E2BIG` | hw_init fails |
| `INIT_IMAGE` / `MEM_SETUP` nonzero x0 or x1 | remapped errno, or positive x1 | hw_init fails |
| `AUTH_AND_RESET` → `-EOPNOTSUPP` (x0 = −4) | `zap_available = false`, returns `-EOPNOTSUPP` | this hw_init fails; the **next** attempt takes the `-ENODEV` fallback path |
| `AUTH_AND_RESET` other error | "Unable to authorize the image" | hw_init fails |

After `INIT_IMAGE` succeeds and a later step fails, Linux does **not** call `PAS_SHUTDOWN`. A retry re-runs `INIT_IMAGE`.

---

## D. Risks / open items
- **R1:** The zap ELF layout is unknown. No `a650_zap.*` exists on this machine. Check `p_flags` bit27 (relocatable) and `p_paddr`, and that size ≤ 8 KiB, before trusting C.5. Non-relocatable images must be linked into `0x8681A000`.
- **R2:** The zap must be the Xiaomi-signed blob from the apollo vendor image. Pick split `.mdt/.bNN` vs a squashed `.mbn` accordingly.
- **R3:** Linux sets the GMU RCG rate **before** enabling the source (gmu:1079-1082). An RCG UPDATE with the GPLL0 div branch unvoted might not complete. Enable GCC `0x152000` bit16 (and GPLL0) first.
- **R4:** GCC MISC `0x171028[1:0]` gates GPLL0 to the GPU unless it is voted. If GPU_CC rails look dead, check these bits and the vote bits.
- **R5:** GX clamp and collapse after a Linux-style stop rely on the GMU firmware to undo them. If the GMU fails to bring GX up, inspect `0x03D91508` and `0x03D9100C`.
- **R6:** Mainline sends no CX/MX/DDR-bandwidth votes for the GPU. Stock does (msm-bus grp3d, cx/mx/gfx arc). Expect possible DDR starvation, or CX too low for the 200 MHz GMU, if nothing else holds those votes.
- **R7:** The SCM WAITQ path cannot work on sm8250 (no IRQ). INTERRUPTED resume must preserve x6.
- **R8:** The upstream AON-reset helper is a no-op (gdsc:243-250 bug). Do not "fix" it blindly.
