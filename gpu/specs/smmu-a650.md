# SM8250 Adreno (A650) GPU SMMU — register-level spec for a minimal XNU driver

Target instance: `adreno_smmu`, ARM CoreLink MMU-500 (SMMUv2 architecture), MMIO
`0x03da0000`, size `0x10000`. Compatible strings: `qcom,sm8250-smmu-500`,
`qcom,adreno-smmu`, `qcom,smmu-500`, `arm,mmu-500`. It is `dma-coherent` and has
`#global-interrupts = <2>`. Masters: the GPU uses SID 0 with mask 0x401, and the GMU
uses SID 5 with mask 0x400.

## Conventions

- Linux tree: `apollo-linux` (royka1/apollo-linux, branch apollo-6.17) (6.17, postmarketOS SM8250 base).
  - `smmu.h` means `drivers/iommu/arm/arm-smmu/arm-smmu.h`.
  - `smmu.c` means `drivers/iommu/arm/arm-smmu/arm-smmu.c`.
  - `impl.c` means `arm-smmu-impl.c`, `qcom.c` means `arm-smmu-qcom.c`, and `qdbg.c` means `arm-smmu-qcom-debug.c`.
  - `iopt.c` means `drivers/iommu/io-pgtable-arm.c`.
  - `iopt.h` means `drivers/iommu/io-pgtable-arm.h`. This file and `include/linux/adreno-smmu-priv.h` are not in the sparse checkout; they were read with `git show HEAD:<path>`.
- Stock Android DT: `kona-v21.dts` = `kona-v21.dts (decompiled stock DTB)`. The stock kernel is msm-4.19: `strings` on the stock boot.img kernel shows `Linux version 4.19.246`, and it contains the option names `qcom,skip-init`, `qcom,use-3-lvl-tables`, `qcom,no-dynamic-asid` and `qcom,actlr`.
- Each fact carries one of three tags:
  - **[SRC]**: verified in the files above (file:line).
  - **[ARCH]**: from the ARM SMMUv2 architecture spec or the MMU-500 TRM, from memory, not in these files. Verify against the TRM.
  - **[INFER]**: my inference.
- Every SMMU register is 32-bit unless it is marked 64.

---

## 1. Register map

### 1.1 Address-space layout (pages, NUMPAGE, CB base)

- The SMMU space is split in two halves. The global half is pages `0..NUMPAGE-1`. The context-bank half is pages `NUMPAGE..2*NUMPAGE-1`, one page per CB. [SRC]
  - Page n is at `base + (n << pgshift)` (smmu.h:479-482).
  - GR0 = page 0 and GR1 = page 1 (smmu.h:516-517).
  - CBn = page `numpage + n` (smmu.h:518).
- Page size comes from `ID1.PAGESIZE` (bit 31): 1 means 64 KB (`pgshift = 16`), 0 means 4 KB (`pgshift = 12`) (smmu.c:1847).
- `NUMPAGE = 1 << (ID1.NUMPAGENDXB[30:28] + 1)` (smmu.c:1850, 1856). The expected total size is `2 * NUMPAGE << pgshift`. Linux only warns if this differs from the DT reg size (smmu.c:1851-1854).
- For this instance [INFER, consistent with every source]:
  - The reg size is 0x10000 with 4 KB pages, so NUMPAGE = 8 (NUMPAGENDXB = 2).
  - The global space is 0x3da0000-0x3da7fff.
  - CB n starts at `0x3da8000 + n*0x1000`.
  - That allows at most 8 CBs. This matches the 8 context IRQs in DT (10 interrupts - 2 global; sm8250.dtsi:3093-3102), and Linux requires `NUMCB <= #context IRQs` on v2 (smmu.c:2193-2199).
  - CB0 = 0x3da8000 = GPU register base 0x3d00000 + **0xa8000**, which is exactly the stock `qcom,gpu-offset = <0xa8000>` (kona-v21.dts:24350). So CB0 is the context bank the CP reaches (see §6).
- Implementation-defined pages inside the global half:
  - QCOM "impl0" registers: `TBU_PWR_STATUS` at base+0x2204, `STATS_SYNC_INV_TBU_ACK` at base+0x25dc and `MMU2QSS_AND_SAFE_WAIT_CNTR` at base+0x2670 (qcom.c:650-659). Linux reads these only through `qcom_scm_io_readl`, a secure IO read (qdbg.c:80-96). Treat them as secure-only and never access them directly.
  - The GFX PRR registers `PRR_CFG_LADDR`/`PRR_CFG_UADDR` at base+0x6008/0x600C (smmu.h:157-158). They are **not** enabled for sm8250: qcom.c:357-362 explicitly excludes `qcom,sm8250-smmu-500`.

### 1.2 GR0 (global space 0, page 0)

| Off | Name | Fields used (Linux) | Source |
|---|---|---|---|
| 0x000 | sCR0 | VMID16EN[31], BSU[15:14], FB[13], PTM[12], VMIDPNE[11], USFCFG[10], GCFGFIE[5], GCFGFRE[4], EXIDENABLE[3], GFIE[2], GFRE[1], CLIENTPD[0] | smmu.h:27-39 |
| 0x010 | sACR (MMU-500 aux config) | CACHE_LOCK[26], S2CRB_TLBEN[10], SMTNMB_TLBEN[8] | smmu.h:42, impl.c:106-108 |
| 0x020 | ID0 | S1TS[30], S2TS[29], NTS[28], SMS[27], ATOSNS[26], PTFS_NO_AARCH32[25], PTFS_NO_AARCH32S[24], NUMIRPT[23:16], CTTW[14], NUMSIDB[12:9], EXIDS[8], NUMSMRG[7:0] | smmu.h:45-57 |
| 0x024 | ID1 | PAGESIZE[31], NUMPAGENDXB[30:28], NUMS2CB[23:16], NUMCB[7:0] | smmu.h:59-63 |
| 0x028 | ID2 | VMID16[15], PTFS_64K[14], PTFS_16K[13], PTFS_4K[12], UBS[11:8], OAS[7:4], IAS[3:0] | smmu.h:65-72 |
| 0x03c | ID7 | MAJOR[7:4], MINOR[3:0] (MMU-500 rXpY) | smmu.h:79-81 |
| 0x048 | sGFSR | USF[1] (Linux names only this bit); write-1-to-clear by writing back the value read | smmu.h:83-84, smmu.c:515 |
| 0x050/54/58 | sGFSYNR0/1/2 | sGFSYNR1[15:0] = offending SID (smmu.c:505-506) | smmu.h:86-88 |
| 0x064 | TLBIVMID | stage-2 only, unused for S1 | smmu.h:91 |
| 0x068 | TLBIALLNSNH | invalidate all non-secure non-hyp entries (reset only) | smmu.h:92, smmu.c:1682 |
| 0x06c | TLBIALLH | invalidate all hyp entries (reset only) | smmu.h:93, smmu.c:1681 |
| 0x070 | sTLBGSYNC | write any value (Linux writes 0xffffffff) | smmu.h:94 |
| 0x074 | sTLBGSTATUS | GSACTIVE[0]; poll until 0 | smmu.h:96-97 |
| 0x800+4n | SMR(n) | VALID[31], MASK[30:16] (field GENMASK(31,16), but bit 31 is VALID when EXIDS is off), ID[15:0] | smmu.h:100-103 |
| 0xc00+4n | S2CR(n) | PRIVCFG[25:24] (0 = default/use incoming, 1 = DIPAN, 2 = unpriv, 3 = priv), TYPE[17:16] (0 = TRANS, 1 = BYPASS, 2 = FAULT), EXIDVALID[10], CBNDX[7:0] | smmu.h:105-120 |

Decoding the ID registers (smmu.c:1739-1937):

- ID0:
  - S1 support comes from `S1TS` (1759-1762).
  - `#SMRs = ID0.NUMSMRG` (only when `SMS` is set, 1808-1825).
  - SID width is `1 << NUMSIDB`, or 16 bits when `EXIDS` is set on v2 (1801-1807).
  - ATOS (`ATS1PR`) is usable when `S1TS && !ATOSNS` (1780-1784).
  - Linux takes coherent walk from DT `dma-coherent`, *not* from `ID0.CTTW`, and only logs a mismatch (1786-1798, 2064-2065).
- ID1 gives `#CBs = NUMCB` and `#S2-only CBs = NUMS2CB` (1858-1863). S1 CB allocation normally starts at `NUMS2CB` (smmu.c:744), but the Adreno impl overrides that (see §2.2).
- ID2 address sizes use `arm_smmu_id_size_to_bits` (smmu.c:1720-1737): 0 = 32, 1 = 36, 2 = 40, 3 = 42, 4 = 44, 5 or more = 48 bits.
  - `ipa_size = IAS(ID2[3:0])`. This is the **S1 output size** used for this domain.
  - `pa_size = OAS(ID2[7:4])`. It is also used as the DMA mask for walker memory (1888).
  - `va_size = UBS(ID2[11:8])`. This is the **S1 input size**.
  - The 4K/16K/64K formats come from `PTFS_*` (1897-1905).
  - The resulting page-size bitmap for 4K is `4K|2M|1G` (1916-1918).
  - [ARCH] On MMU-500, UBS = 0b0101 means a 49-bit upstream bus, which Linux maps to 48. Bit 48 is the TTBR0/TTBR1 sign bit. This is why msm sign-extends from bit 48 (msm_iommu.c:679-681).
- Qcom applies no ID tweaks to this instance: `qcom_adreno_smmu_500_impl` has **no** `.cfg_probe` (qcom.c:607-615). The 16K-format removal in `qcom_adreno_smmuv2_cfg_probe` (qcom.c:488-499) applies only to the v2 Adreno SMMUs.

### 1.3 GR1 (global space 1, page 1)

| Off | Name | Fields | Source |
|---|---|---|---|
| 0x000+4n | CBAR(n) | IRPTNDX[31:24] (v1 only, so Linux leaves it 0 on v2: smmu.c:606-607), TYPE[17:16] (0 = S2, **1 = S1 trans + S2 bypass**, 2 = S1 + S2 fault, 3 = nested), S1 MEMATTR[15:12] (WB = 0xf), S1 BPSHCFG[9:8] (NSH = 3), VMID[7:0] (S2/8-bit VMID only) | smmu.h:123-136 |
| 0x400+4n | CBFRSYNRA(n) | SID[15:0] of the faulting stream | smmu.h:138-139 |
| 0x800+4n | CBA2R(n) | VMID16[31:16], VA64[0] | smmu.h:141-143 |

On v2 the context IRQ index equals cbndx (smmu.c:795-800). So CB n uses context IRQ n, which is DT interrupt `2+n`: CB0 = GIC SPI 678, CB1 = SPI 679, and so on. The global IRQs are SPI 672 and 673 (sm8250.dtsi:3093-3102; stock kona-v21.dts:9759 uses the same 0x2a0, 0x2a1 and 0x2a6.. values).

### 1.4 Context-bank space (page NUMPAGE+n)

| Off | Name | Fields | Source |
|---|---|---|---|
| 0x000 | SCTLR | S1_ASIDPNE[12], HUPCF[8], CFCFG[7] (1 = stall on fault), CFIE[6], CFRE[5], E[4] (big-endian), AFE[2], TRE[1], M[0] | smmu.h:145-154 |
| 0x004 | ACTLR | impl-defined (see §1.5) | smmu.h:156 |
| 0x008 | RESUME | TERMINATE[0]: 0 = retry, 1 = abort the stalled transaction | smmu.h:160-161 |
| 0x010 | TCR2 | SEP[17:15] (Linux writes 7 = "upstream"), AS[4] (16-bit ASID), PASIZE[3:0] | smmu.h:163-167 |
| 0x020 | TTBR0 (64) | ASID[63:48] \| table PA | smmu.h:169-171 |
| 0x028 | TTBR1 (64) | ASID[63:48] \| table PA | smmu.h:170-171 |
| 0x030 | TCR | AArch64 layout: TG1[31:30], SH1[29:28], ORGN1[27:26], IRGN1[25:24], EPD1[23], A1[22], T1SZ[21:16], TG0[15:14], SH0[13:12], ORGN0[11:10], IRGN0[9:8], EPD0[7], T0SZ[5:0]. Bit 31 doubles as EAE only for AArch32-L. | smmu.h:173-182, 395-414 |
| 0x034 | CONTEXTIDR | not written by Linux for AArch64 (only for v7s: smmu.c:634-637). The CP writes 0 (a6xx_gpu.c:168). | smmu.h:193 |
| 0x038 / 0x03c | MAIR0 / MAIR1 | | smmu.h:194-195 |
| 0x050 | PAR (64) | F[0]; PA[39:12] used (smmu.c:1377) | smmu.h:197-198 |
| 0x058 | FSR | MULTI[31], SS[30], FORMAT[10:9], UUT[8], ASF[7], TLBLKF[6], TLBMCF[5], EF[4], PF[3], AFF[2], TF[1]. Write-1-to-clear. | smmu.h:200-224 |
| 0x060 | FAR (64) | faulting input VA | smmu.h:226 |
| 0x068 | FSYNR0 | S1CBNDX[23:16], AFR[11], PTWF[10], NSATTR[8], IND[6], PNU[5], WNR[4], PLVL[1:0] | smmu.h:228-236 |
| 0x06c | FSYNR1 | impl-defined; on A6xx, [7:0] is the GPU client ID (see §5) | smmu.h:238, a6xx_gpu.c:1647 |
| 0x600 | TLBIVA (64) | `(VA>>12) \| ASID<<48` | smmu.h:240 |
| 0x610 | TLBIASID | ASID | smmu.h:241 |
| 0x620 | TLBIVAL (64) | last-level-only variant of TLBIVA | smmu.h:242 |
| 0x7f0 | TLBSYNC | write any value | smmu.h:245 |
| 0x7f4 | TLBSTATUS | SACTIVE[0] (same bit as GSACTIVE) | smmu.h:246, smmu.c:226 |
| 0x800 | ATS1PR (64) | ATOS stage-1 privileged read | smmu.h:247 |
| 0x8f0 | ATSR | ACTIVE[0] | smmu.h:249-250 |

Registers that Linux does not define but the architecture does [ARCH]:

- `TLBIALL` at CB+0x618: invalidates all entries for this CB.
- `TLBIVAA` at 0x608 and `TLBIVAAL` at 0x628.
- `FSRRESTORE` at 0x05c.
- `IPAFAR` at 0x070.

TLBIALL is a useful alternative to TLBIASID before first enabling a CB.

### 1.5 MMU-500 implementation-specific bits

- **sACR** (GR0+0x10) [SRC impl.c:104-146]:
  - `CACHE_LOCK` (bit 26): on MMU-500 r2p0+ (`ID7.MAJOR >= 2`), CB ACTLR writes only stick when it is clear. Linux clears it and says it "hope[s] that Secure has also cleared SACR.CACHE_LOCK" (impl.c:113-117).
  - `S2CRB_TLBEN` (bit 10) and `SMTNMB_TLBEN` (bit 8): Linux sets both ("Allow unmatched Stream IDs to allocate bypass TLB entries", impl.c:123-128).
  - No MMUDIS bit is defined anywhere in these sources. Do not touch it.
- **CB ACTLR** [SRC qcom.c:19-41]:
  - Bit 0 = CMTLB (context caching in the macro TLB).
  - Bit 1 = CPRE (context caching in the prefetch buffer).
  - Bits [9:8] = prefetch depth (0 = default, 1 = shallow, 2 = moderate, 3 = deep).
  - Bit 5 = `GFX_ACTLR_PRR` on newer Adreno SMMUs only (qcom.c:33). It is not used on sm8250 (qcom.c:357-362).
  - Upstream writes `PREFETCH_DEEP|CPRE|CMTLB = 0x303` for clients `qcom,adreno`, `qcom,adreno-gmu` and `qcom,adreno-smmu` (qcom.c:36-41, 299-311, 325-328).
  - The stock DT value is `0x32b` (kona-v21.dts:9763: `qcom,actlr = <SID mask value>` triples for SIDs 2, 4, 5, 7 and 0). 0x32b = 0x303 | bit 3 | bit 5. In upstream terms that is CMTLB + CPRE + deep prefetch + bit 3 (undocumented here) + bit 5 (PRR on newer parts). The meaning of bit 3 cannot be derived from these sources.
  - MMU-500 errata: with `CONFIG_ARM_SMMU_MMU_500_CPRE_ERRATA` (=y in the apollo config, `Documentation/apollo/config-apollo-6.17:6561`), reset clears CPRE in every CB's ACTLR (impl.c:130-143). `init_context` later rewrites 0x303 for the GPU and GMU CBs, which re-enables CPRE there.
  - **Recommendation:** write 0x303, or 0 (no prefetch, the safest for errata). Read it back; if it did not stick, CACHE_LOCK is set and only performance is affected.
- **TBUs** (Qualcomm wrapper) [SRC kona-v21.dts:9766-9778, qdbg.c:22-41]:
  - `gfx_0_tbu` @0x3dc5000 serves SIDs 0x000-0x3ff and `gfx_1_tbu` @0x3dc9000 serves SIDs 0x400-0x7ff. The TCU is at 0x3dc2000, and the TBU status registers are at 0x3dc2200 and 0x3dc2208.
  - So SID bit 10 selects the TBU. That is why every Adreno SMR mask contains 0x400 [INFER].
  - TBU debug registers (Linux uses them only for ECATS/halt debug):
    - `SID_HALT` +0x0: HALT[16], SID[9:0].
    - `VA_ADDR` +0x8.
    - `TXN_TRIGG` +0x18.
    - `SR_HALT_ACK` +0x20: HALT_ACK[1], ECATS_RUNNING[0].
    - `PAR` +0x28.
    - `AXUSER` +0x30.
  - A minimal driver needs none of these. Upstream sm8250 DT has no TBU nodes for this SMMU.

---

## 2. What Linux does (probe/reset/attach) and what the Qualcomm/Adreno impl changes

### 2.1 Generic probe and reset (arm-smmu.c)

1. **Matching.** `arm,mmu-500` gives version v2 and model `ARM_MMU500` (smmu.c:1950, 1959). The impl chain is `arm_mmu500_impl`, then qcom (impl.c:204-224). `qcom,sm8250-smmu-500` matches `qcom_smmu_500_impl0_data` (qcom.c:717). Because the node is also `qcom,adreno-smmu`, the **`qcom_adreno_smmu_500_impl`** is selected (qcom.c:627-630, 684-689, 607-615).
2. **`cfg_probe`** reads ID0/1/2 as in §1.2 (smmu.c:2189).
3. **IRQs.** It requests the 2 global IRQs (smmu.c:2210-2222).
4. **RMR bypass.** `arm_smmu_rmr_install_bypass_smr` is ACPI/IORT only, so it is a no-op on DT apart from **setting `sCR0.CLIENTPD=1`** (smmu.c:2086-2088).
5. **`arm_smmu_device_reset`** (smmu.c:1658-1718). In order:
   - Clear sGFSR (write back the value read).
   - **Rewrite every SMR and S2CR** from the software state (`arm_smmu_write_sme(i)` for all i). For the Adreno SMMU the software state is "all invalid": no handoff, because there is no cfg_probe. Unused S2CRs become FAULT when `disable_bypass` (smmu.c:60-68, 1671-1672). `CONFIG_ARM_SMMU_DISABLE_BYPASS_BY_DEFAULT=y` on apollo (config:6560).
   - For **every** CB, `arm_smmu_write_context_bank`: unassigned CBs get `SCTLR=0`. Then write `CB_FSR = FSR_FAULT` (smmu.c:1674-1678).
   - Write `TLBIALLH` and `TLBIALLNSNH` with value 0xffffffff (smmu.c:1681-1682).
   - Compute a new sCR0 from its current value: set GFRE|GFIE|GCFGFRE|GCFGFIE and VMIDPNE|PTM; clear CLIENTPD; set USFCFG = `disable_bypass`; clear FB; BSU = 0; set VMID16EN if `ID2.VMID16`; set EXIDENABLE if `ID0.EXIDS` (smmu.c:1684-1710).
   - Run `impl->reset`, which is `arm_mmu500_reset`: the sACR changes and the CPRE errata loop (smmu.c:1712-1713, impl.c:110-146).
   - Run a global TLB sync, then write sCR0 (smmu.c:1716-1717).
6. **`arm_smmu_test_smr_masks`.** In the first invalid SMR, write ID = all ones and read it back, then write MASK = all ones and read it back. This yields the implemented SID and mask bits (smmu.c:995-1029).
7. **Runtime PM.** *Every* runtime resume calls `arm_smmu_device_reset` again (smmu.c:2289-2301). This reprograms all SMEs and all assigned CBs from software state. The SMMU sits in `GPU_CX_GDSC` and needs the `GPU_CC_AHB`, `GCC_GPU_MEMNOC_GFX` and `GCC_GPU_SNOC_DVM_GFX` clocks (sm8250.dtsi:3103-3108), so its register state can be lost on CX collapse.
8. **Shutdown** writes `sCR0 = CLIENTPD` (smmu.c:2268).

Per-master steps:

- **`probe_device`** checks `sid & ~streamid_mask` and `mask & ~smr_mask_mask` (smmu.c:1454-1468). It then allocates SMEs with `arm_smmu_find_sme` (smmu.c:1031-1071):
  - An existing SMR that fully covers the new (id, mask) is reused.
  - Any partial overlap is rejected with -EINVAL.
  - Otherwise the first invalid index is used.
- It writes them immediately (S2CR first, then SMR: smmu.c:984-989, 1118-1120). At that point the S2CR still has its init type (FAULT).
- The DT `iommus = <&adreno_smmu SID MASK>` cells are packed as fwid = ID[15:0] | MASK[31:16] (smmu.c:1587-1601).
- SMR encoding (smmu.c:953-962): `ID | MASK<<16 | VALID(bit31, only if !EXIDS)`.
- S2CR encoding (smmu.c:964-982): `TYPE<<16 | CBNDX | PRIVCFG<<24`, plus `EXIDVALID` when EXIDS is on.
- **`attach_dev`** runs `init_domain_context`, then `install_s2crs(TRANS, cbndx)` with `PRIVCFG_DEFAULT` (smmu.c:1168-1213, 1149-1166).

`init_domain_context` (smmu.c:675-886):

- **Stage and format.** S1 and AArch64. It sets `cbar = CBAR_TYPE_S1_TRANS_S2_BYPASS`, `ias = va_size`, `oas = ipa_size` (smmu.c:742-758).
- **CB.** `alloc_context_bank` is overridden by the impl (§2.2). Then `irptndx = cbndx` (v2) and **`asid = cbndx`** (smmu.c:787-805).
- **Page-table config.** `pgtbl_cfg = {pgsize_bitmap, ias, oas, coherent_walk = FEAT_COHERENT_WALK, tlb = s1 ops}` (smmu.c:807-814).
- **Impl hook.** `impl->init_context` writes ACTLR and may set the TTBR1 quirk (smmu.c:816-820). Any domain quirks are added on top (smmu.c:822-823).
- **Aperture.** With TTBR1: `aperture = [~0UL << ias, ~0UL]`. Otherwise `[0, 2^ias - 1]` (smmu.c:834-841).
- **CB register values** (`init_context_bank`, smmu.c:519-574):
  - `tcr[0] = arm_smmu_lpae_tcr()`.
  - `tcr[1] = TCR2 (PASIZE = ips, SEP = 7) | AS`.
  - Both TTBRs carry the ASID in [63:48]. The table PA goes in TTBR1 with the quirk, otherwise in TTBR0.
  - `mair[0..1]` = the 64-bit MAIR split into two halves.
- **CB write order** (`write_context_bank`, smmu.c:576-663):
  1. CBA2R = VA64 | (VMID16 = `cfg->vmid` if FEAT_VMID16). `vmid` is a union with `asid`, so it equals cbndx (smmu.h:355-358).
  2. CBAR = TYPE(1) | BPSHCFG(NSH = 3) | MEMATTR(WB = 0xf) = **0x0001F300**.
  3. TCR2, then TCR. The comment says this must come before the TTBRs because it governs ASID[15:8].
  4. TTBR0 (64), then TTBR1 (64).
  5. MAIR0, then MAIR1.
  6. Last, SCTLR = CFIE|CFRE|AFE|TRE|M|S1_ASIDPNE = **0x1067**, passed through `impl->write_sctlr`.
- **Context IRQ.** It requests the context IRQ as a threaded handler, because the impl sets `context_fault_needs_threaded_irq` (smmu.c:851-872). The handler is the generic `arm_smmu_context_fault`: the Adreno impl defines no `.context_fault`, and the qcom debug handler is only in `qcom_smmu_500_impl` (qcom.c:571-582 vs 607-615).

### 2.2 Qualcomm/Adreno impl differences (qcom.c)

- **Context-bank assignment** (`qcom_adreno_smmu_alloc_context_bank`, qcom.c:268-287):
  - Any master whose fwspec has **SID 0** is "the GPU" (qcom.c:191-210). It gets **CB0 only** (start = 0, count = 1). If CB0 is taken, allocation fails.
  - Everything else, including the GMU (SID 5), gets the first free CB ≥ 1. So the GMU gets CB1.
  - Reason given: "Assign context bank 0 to the GPU device so the GPU hardware can switch pagetables" (qcom.c:274-276).
  - drm/msm also tells TZ about CB0: `qcom_scm_set_gpu_smmu_aperture(0)`, "We currently always use context bank 0, so hard code this" (adreno_gpu.c:662-668).
  - The SCM call is SIP SMC, svc `QCOM_SCM_SVC_MP` = 0x0c, cmd `QCOM_SCM_MP_CP_SMMU_APERTURE_ID` = 0x1b. Its args are `0xffff0000 | cb`, 0xffffffff, 0xffffffff, 0xffffffff (qcom_scm.c:910-934, qcom_scm.h:116, 123).
  - ASIDs: GPU CB0 → ASID 0, GMU CB1 → ASID 1 (smmu.c:805).
- **`qcom_adreno_smmu_init_context`** (qcom.c:313-365):
  - Sets `flush_walk_prefer_tlbiasid = true`.
  - Writes ACTLR = 0x303 for GPU and GMU (client match).
  - For the GPU only, it sets **`IO_PGTABLE_QUIRK_ARM_TTBR1`** when the domain is S1 AArch64 and the SMMU is not msm8996. The kernel/global GPU page table therefore lives in **TTBR1** and TTBR0 starts disabled (EPD0 = 1).
  - It fills `adreno_smmu_priv`: `get_ttbr1_cfg`, `set_ttbr0_cfg`, `get_fault_info`, `set_stall`. PRR callbacks stay NULL on sm8250.
- **`set_ttbr0_cfg`** (qcom.c:227-266) handles per-process page tables.
  - It requires split mode (EPD1 clear).
  - On enable: `tcr |= lpae_tcr(ttbr0_cfg)`; clear EPD0 and EPD1; `ttbr0 = pgd | ASID<<48`; then rewrite the whole CB (`arm_smmu_write_context_bank`).
  - On disable: restore the TTBR1-only TCR and `ttbr0 = ASID<<48`.
  - msm calls it once, when the first per-process page table is created, and again when the last one is destroyed (msm_iommu.c:575-605, 232-235). After that the CP swaps TTBR0 itself (§6).
- **`write_sctlr`** (qcom.c:78-93):
  - Always ORs **HUPCF** ("process subsequent transactions after a fault to keep the GPU from hanging").
  - ORs **CFCFG** (stall) if this CB's bit is set in `stall_enabled`.
  - This applies to *all* CBs of this SMMU, including the GMU's.
- **`set_stall`** (qcom.c:111-148) flips SCTLR.CFCFG live on the GPU CB. msm enables stall right after creating the GPU domain (msm_iommu.c:789-791). On a fault, the GPU fault handler turns stall off for at least 500 ms (adreno_gpu.c:303-315).
- **`get_fault_info`** reads FSR, FSYNR0, FSYNR1, FAR (64), CBFRSYNRA(cb), TTBR0 (64) and CONTEXTIDR (qcom.c:95-109).
- **TLB sync** (`qcom_smmu_tlb_sync`, qcom.c:58-76): same as generic. On timeout it dumps the TBU status registers through SCM (qdbg.c:64-102).
- **Handoff.** `qcom_smmu_cfg_probe` is the "SMR handoff": it reads each SMR left valid by the bootloader and keeps it as valid + BYPASS. It also caps the SMR count at 128 and detects the "S2CR BYPASS → FAULT" firmware quirk (qcom.c:410-486). It is wired **only to the non-Adreno `qcom_smmu_500_impl`** (qcom.c:571-577). **For the Adreno SMMU, upstream Linux does no handoff.** Its reset invalidates every SMR, writes every S2CR, and disables every CB that it does not own.
  - The bypass-quirk comment, "With some firmware versions writes to S2CR of type FAULT are ignored, and writing BYPASS will end up written as FAULT" (qcom.c:446-450), shows that the firmware **filters or emulates** S2CR writes on Qualcomm parts.
- **Hypervisor trapping.** The arm-smmu.c header comment says: "some Qualcomm arm64 platforms which appear to expose their SMMU global register space are still, in fact, using a hypervisor to mediate it by trapping and emulating register accesses ... bugs ... for stores using r31 (i.e. XZR/WZR) as the source register". Every constant write (sync/TLBI) therefore uses `QCOM_DUMMY_VAL = -1` (smmu.c:44-51; qcom.c:17, 64).
- **No `qcom,smmu-500` reset hook.** The `.reset` for this impl is plain `arm_mmu500_reset` (qcom.c:610). The SDM845-only `qcom_sdm845_smmu500_reset` adds an SCM wait-for-safe toggle (qcom.c:544-561) but is not used here.

### 2.3 What the stock (downstream) DT says

The stock DT changes the SMMU node as follows:

- `kgsl-smmu@3da0000` (kona-v21.dts:9743-9779) uses `compatible = "qcom,qsmmu-v500"`. Its `reg` includes `tcu-base 0x3dc2000`. It sets `qcom,skip-init`, `qcom,use-3-lvl-tables` and `qcom,no-dynamic-asid`.
- It has **no `dma-coherent`**. The only coherent node is the disabled `kgsl_iommu_coherent_test_device`, SID 9 (kona-v21.dts:9948-9953).

The client nodes are:

- `qcom,kgsl-iommu@3da0000` (kona-v21.dts:24335-24361):
  - `qcom,protect = <0xa0000 0xc000>`.
  - `qcom,hyp_secure_alloc`.
  - `qcom,retention`.
  - `gfx3d_user` = SID 0, mask 0x401, `qcom,gpu-offset 0xa8000`.
  - `gfx3d_secure` = SID 2, mask 0x400.
- The GMU node (kona-v21.dts:24384-24395): `gmu_user` = SID 4/0x400 and `gmu_kernel` = SID 5/0x400.
- `kgsl_iommu_test_device` = SID 7 (kona-v21.dts:9942-9946).

**What `qcom,skip-init` means downstream.** This is [DOWNSTREAM-recall from msm-4.19 `arm-smmu.c`]; the source is not in the provided files. The option sets `ARM_SMMU_OPT_SKIP_INIT`. In `arm_smmu_device_reset()`, the "reset stream mapping groups" loop and the context-bank disable/FSR-clear loop are then skipped. The downstream driver also has its own SMR/S2CR/CB "handoff" that adopts entries the bootloader or hypervisor left valid.

Implications for us [INFER]:

1. Firmware (XBL/QHEE) may leave **valid SMR and S2CR entries and live CBs** in this SMMU when the kernel starts. Downstream deliberately leaves them alone.
2. The global registers sCR0 and sACR are already configured: CLIENTPD and USFCFG are set appropriately, VMID16EN and EXIDENABLE are already decided, and CACHE_LOCK is handled by secure world.
3. Global register writes go through QHEE trap-and-emulate (smmu.c:44-51), which can ignore or rewrite them (qcom.c:446-450).

Two more downstream options:

- `qcom,use-3-lvl-tables` [DOWNSTREAM-recall] limits ias to 39 bits, giving 3-level 4K tables.
- `qcom,no-dynamic-asid` [DOWNSTREAM-recall] makes KGSL/arm-smmu use the CB's fixed ASID instead of allocating per-pagetable ASIDs. Upstream behaves the same way, with all GPU page tables on ASID 0 (msm_iommu.c:613-619).

`qcom,hyp_secure_alloc` [INFER] means the secure CB's page-table memory is hyp-assigned (stage-2 protected) to the content-protection VM. The downstream kernel still programs the secure CB at runtime when KGSL probes, so the firmware does not pre-program SID 2 for us. There is no `qcom,tz-device-id`, so it is not a "slave-side secure" SMMU owned by TZ.

Upstream evidence that a full reset is survivable:

- Mainline arm-smmu fully resets this SMMU (§2.1). The apollo tree enables the GPU with the unmodified mainline driver (sm8250-xiaomi-apollo.dts:612-619).
- However, the README status table does not list the GPU, so "GPU works on apollo with full reset" is **not confirmed** by these sources.

Possibly hyp-owned or trapped resources:

| Resource | Status |
|---|---|
| GR0 global space (sCR0, sACR, SMR, S2CR, global TLBI/sync, sGFSR) | Trapped and emulated by QHEE (smmu.c:44-51). S2CR writes may be filtered (qcom.c:446-450). |
| GR1 (CBAR, CBA2R) | Probably also mediated [INFER]. |
| CB pages | Probably direct [INFER]. The CP writes CB0 directly (§6). |
| impl0 TBU/TCU status (base+0x2000 page) | Secure-only, read through SCM (qdbg.c:80-96). |
| CP access to CB0 | Gated by TZ through the SCM aperture call (adreno_gpu.c:662-668). |
| Secure CB for SID 2 (`gfx3d_secure`) | Hyp/secure-side memory. [ARCH] On MMU-500 with security extensions, secure-only CBs are RAZ/WI to non-secure accesses. |

### 2.4 Recommended minimal, non-destructive programming sequence

**Rules for MMIO accesses** (on a hypervisor-trapped region):

- Use single 32-bit `LDR`/`STR` with no writeback or pair forms.
- [ARCH] Trapped accesses need `ESR.ISV = 1`. LDP/STP or pre/post-index accesses cannot be emulated.
- **Never store from WZR/XZR.** Linux's `writel` uses an `"rZ"` constraint, hence `QCOM_DUMMY_VAL`. In XNU, use inline asm with an `"r"` constraint for every SMMU store, even when the value is 0. Note that the GPU's S2CR value (TRANS, CB0) is **exactly 0x00000000**.
- Use 64-bit `STR` only for CB TTBR0/TTBR1/TLBIVA(L) and `LDR` for FAR/PAR. MMU-500 supports 64-bit accesses; Linux uses `writeq_relaxed` (smmu.h:507-514).

**Steps:**

0. **Power.** Turn on `GPU_CX_GDSC` and enable the `GPU_CC_AHB_CLK`, `GCC_GPU_MEMNOC_GFX_CLK` and `GCC_GPU_SNOC_DVM_GFX_CLK` clocks (plus an interconnect vote) before any access (sm8250.dtsi:3103-3108).
1. **Read-only discovery.** Read ID0, ID1, ID2, ID7 and sCR0. Check `pgshift = 12`, `NUMPAGE = 8` and `NUMCB ≤ 8`. Record EXIDENABLE and VMID16EN from the **live sCR0**, not from ID0/ID2, because the SMR/S2CR/CBA2R formats depend on them (smmu.c:959-960, 978-980, 598-599).
2. **Snapshot.** Read all `SMR(i)` and `S2CR(i)` for `i < NUMSMRG` (cap at 128: qcom.c:439-442). For each CB, read CBAR, CBA2R and SCTLR (M bit). Log everything. This is the "handoff" state.
3. **Choose SMEs** with Linux `find_sme` semantics (smmu.c:1031-1071) for (0, 0x401) and (5, 0x400).
   - Reuse a valid SMR that fully covers the request.
   - **Abort** on partial overlap with a foreign SMR.
   - Otherwise take a free (invalid) SMR index.
   - Never modify a foreign valid SMR or its S2CR, especially one matching SID 2, 4 or 7.
   - (0, 0x401) covers SIDs {0, 1, 0x400, 0x401}. (5, 0x400) covers {5, 0x405}. SID 2 with mask 0x400 covers {2, 0x402}. There is no overlap.
4. **Choose CBs.**
   - GPU: **CB0** (mandatory for the CP, §6). Proceed only if no foreign S2CR points at CB0 (TYPE = TRANS, CBNDX = 0) and `CB0.SCTLR.M == 0`.
   - GMU: the lowest CB ≥ 1 with `SCTLR.M == 0` that is not referenced by a foreign S2CR. That is CB1 on Linux.
5. **Program each CB** while SCTLR.M = 0, in the Linux order:
   1. Write `SCTLR = 0`.
   2. Clear FSR by writing 0xC00001FE (= `FSR_FAULT`; smmu.h:218-224, smmu.c:1677).
   3. Write ACTLR (0x303, or leave it).
   4. Write CBA2R.
   5. Write CBAR = 0x0001F300.
   6. Write TCR2.
   7. Write TCR.
   8. Write TTBR0 and TTBR1.
   9. Write MAIR0 and MAIR1.
   10. Write `TLBIASID = asid` (or `TLBIALL` [ARCH]), then `TLBSYNC = 0xffffffff`, and poll `TLBSTATUS[0] == 0`.
   11. Write the final SCTLR.

   Values are in §3.6.
6. **Install streams.** Write **S2CR first** (TYPE = TRANS, CBNDX, PRIVCFG = 0, plus EXIDVALID if EXIDENABLE), **then SMR** (ID | MASK<<16 | VALID). This is the `write_sme` order (smmu.c:984-989). **Read both back.** A mismatch means the hypervisor filtered the write (qcom.c:446-458); stop if that happens.
7. **Do NOT:**
   - write all SMEs or disable all CBs (the `device_reset` loops);
   - issue `TLBIALLNSNH`/`TLBIALLH`;
   - write sACR;
   - run `test_smr_masks` (it scribbles a free SMR; instead check that SID/mask bits fit `NUMSIDB`).
8. **sCR0.** If the live sCR0 has `CLIENTPD == 0`, **leave it alone**. If `CLIENTPD == 1`, the whole SMMU is bypassing and our CBs will never be used, so clearing it is mandatory.
   - Do it as a read-modify-write that only clears CLIENTPD and keeps everything else, including USFCFG.
   - Risk: every other stream (SID 2, 4, 7, …) suddenly becomes subject to SMR matching and USFCFG. Unmatched streams then fault (USFCFG = 1) or bypass (USFCFG = 0). The hypervisor may also reject the write.
   - Optional and lower risk: set GFRE/GFIE so global faults are reported. Linux sets these plus GCFGFRE/GCFGFIE (smmu.c:1687-1688).
   - Do not change VMID16EN, EXIDENABLE, PTM, VMIDPNE or BSU; that would reinterpret other owners' entries.
9. **IRQs.** Hook the context IRQ for CB0 (SPI 678) and CB1 (SPI 679), and the global IRQs (SPI 672/673). sGFSR: read and log, and clear only the bits seen (write-1-to-clear, smmu.c:515).
10. **CP aperture.** Before using `CP_SMMU_TABLE_UPDATE`, issue the SCM call that sets the CP SMMU aperture to CB0 (§2.2).
11. **On every CX power-up** (runtime resume): re-read our SMR/S2CR/CB0/CB1. If anything was lost (Linux assumes it can be, smmu.c:2289-2301), redo steps 5-6. Also invalidate the TLB for each ASID on power-up.

**Global registers we might still touch, and the risk of each:**

- **sCR0.CLIENTPD**: only if it is set. The risk is above.
- **sCR0.GFRE/GFIE**: changes only reporting. Low risk, but QHEE may emulate or ignore it.
- **sGFSR clear**: low risk, but it may erase evidence the hypervisor also reads.
- **SMR/S2CR (our indices only)**: required. If the hypervisor rejects them, translation never happens.
- **sACR.CACHE_LOCK**: not needed. Without it, ACTLR may not stick, which costs only performance.

---

## 3. Stage-1 page-table format (io-pgtable-arm, `ARM_64_LPAE_S1`)

### 3.1 Granule, sizes and levels

- **Granule.** It is the CPU `PAGE_SIZE` if supported, otherwise the next best (iopt.c:875-916). Linux arm64 uses 4 KB, so the page sizes are **4K | 2M | 1G** (iopt.c:897-899).
  - XNU uses 16 KB CPU pages, but **use the 4 KB granule anyway**. Qcom says 16K "doesn't seem to work" on some Adreno SMMUs (qcom.c:490-491). Map each 16 KB XNU page as 4 contiguous 4 KB PTEs.
- **Level math** (iopt.c:918-958):
  - `bits_per_level = 12 - 3 = 9`.
  - `levels = ceil((ias - 12) / 9)`.
  - `start_level = 4 - levels`.
  - `pgd_bits = (ias - 12) - 9*(levels - 1)`.
  - The PGD is allocated at a minimum of 64 bytes (iopt.c:266).
- **ias / oas Linux uses:**
  - GPU domain and GMU domain: `ias = va_size` (ID2.UBS: 48 if UBS ≥ 5) and `oas = ipa_size` (ID2.IAS) (smmu.c:745-746).
  - Both are clamped to ≤ 48 for 4K (iopt.c:913-915).
  - The per-process TTBR0 tables clone the TTBR1 cfg, so they have the same ias/oas (msm_iommu.c:526-531).
  - [INFER] On MMU-500 that means ias = 48 (4 levels, start level 0, 512-entry PGD). oas must be read from ID2.IAS. Typical Qualcomm MMU-500 logs show a 36-bit IPA, but **verify**.
- **ias choice for us:**
  - `ias = 48`: 4 levels, PGD = 1 page.
  - `ias = 39` (what downstream `qcom,use-3-lvl-tables` implies): 3 levels, start level 1, `pgd_bits = 9`, PGD = 1 page.
  - **Recommendation: ias = 39** for both CBs. It is simpler and matches downstream. The CP never touches TCR (§6), so any T0SZ/T1SZ works as long as TTBR0 tables match TCR.
  - Keep `oas` = ID2.IAS bits, and all table/page PAs must be below 2^oas (iopt.c:567).
- **GPU VA layout upstream:**
  - The kernel/global VM is in TTBR1: `aperture_start = ~0 << ias` (smmu.c:834-836). drm/msm uses `start = max(16M, aperture_start) & GENMASK(48, 0)` (adreno_gpu.c:221-230), so with ias = 48 the GPU sees 0x1_0000_0000_0000-0x1_ffff_ffff_ffff.
  - `msm_iommu_map` sign-extends bit 48 to bits [63:49] before the iommu call (msm_iommu.c:679-681).
  - User VMs are in TTBR0 from `ADRENO_VM_START = 0x1_0000_0000` (adreno_gpu.h:551) up to `2^ias` (adreno_gpu.c:238-262, a6xx_gpu.c:2280-2290).
  - A650 (chip 0x06050002) has no `ADRENO_QUIRK_4GB_VA` (a6xx_catalog.c:964-976).
  - With ias = 39, the TTBR1 window as seen by the GPU is 0x1_ff80_0000_0000-0x1_ffff_ffff_ffff [INFER from 49-bit sign extension].
- **GMU domain:**
  - It uses TTBR0 only, with VA range `0 .. 0x80000000` (a6xx_gmu.c:1316-1331).
  - Fixed allocations (A650):
    - `dummy` at 0x60000000 (4K).
    - `icache` at 0x4000, size 16M-16K, mapped PRIV.
    - Uncached allocations (log, hfi) at 0x60001000-0x80000000.
  - Sources: a6xx_gmu.c:1956-2014, 1284-1295.
  - 32-bit ias would suffice for the GMU. ias = 39 is fine.

### 3.2 TCR / TCR2 values

TCR field values come from `arm_64_lpae_alloc_pgtable_s1` (iopt.c:979-1034), with the encodings in iopt.h:5-28:

- Coherent walk (`dma-coherent`): `SH = IS (3)`, `IRGN = ORGN = WBWA (1)` (iopt.c:980-985).
- Non-coherent walk: `SH = OS (2)`, `IRGN = ORGN = NC (0)` (iopt.c:986-993).
- `TG`: TG0_4K = 0. With the TTBR1 quirk, TG1_4K = 2 (iopt.c:995-999).
- `IPS`: 32 = 0, 36 = 1, 40 = 2, 42 = 3, 44 = 4, 48 = 5 (iopt.c:1008-1032). It goes into **TCR2.PASIZE** (smmu.h:416-420).
- `TSZ = 64 - ias` (iopt.c:1034).

Packing (`arm_smmu_lpae_tcr`, smmu.h:395-414):

- `low16 = TG<<14 | SH<<12 | ORGN<<10 | IRGN<<8 | TSZ`.
- With the TTBR1 quirk: `TCR = (low16 << 16) & ~A1 | EPD0`.
- Without it: `TCR = low16 | EPD1`.
- **TBI is never set** by Linux.
- AArch64 has no EAE (smmu.c:535-538).
- `TCR2 = SEP(7)<<15 | AS(1)<<4 | PASIZE` = **0x00038010 | ips**. For example, a 36-bit oas gives 0x00038011.

Precomputed values (coherent walk):

| CB / state | ias = 48 | ias = 39 |
|---|---|---|
| GPU CB0, TTBR1 only (as attached; EPD0 = 1) | `0xB5100080` | `0xB5190080` |
| GPU CB0, TTBR1 + TTBR0 (after `set_ttbr0_cfg`; EPD0 = EPD1 = 0) | `0xB5103510` | `0xB5193519` |
| GPU CB0, single shared table in TTBR0 only (first-cut option A, §6) | `0x00803510` | `0x00803519` |
| GMU CB1, TTBR0 only (EPD1 = 1) | `0x00803510` | `0x00803519` |

For a non-coherent walk, replace the SH/RGN nibble 0x35 with 0x20. For example, GPU TTBR1-only at ias = 48 becomes `0xA0100080`.

### 3.3 TTBRs / ASID

- TTBRn = table PA | `ASID << 48` (smmu.c:549-559). Linux writes the ASID into both TTBRs.
- Because `A1 = 0`, the ASID is taken from **TTBR0**, even when TTBR0 is disabled (EPD0) [ARCH].
- GPU CB0:
  - ASID 0. `TTBR1 = pgd_pa`. `TTBR0 = 0` (EPD0), or the per-process pgd with ASID 0 (qcom.c:258-260; msm_iommu.c:613-619).
  - Upstream gives every process page table the **same ASID 0** ("TODO ... unique ASID", msm_iommu.c:613-619).
- GMU CB1: `TTBR0 = pgd_pa | 1<<48`, `TTBR1 = 1<<48`.
- `SCTLR.S1_ASIDPNE` makes ASIDs private per CB (smmu.c:654-655), so ASID 0 on CB0 and ASID 1 on CB1 never alias with other CBs.

### 3.4 MAIR

MAIR (iopt.c:129-138, 1036-1046): `MAIR = 0x44 | 0xFF<<8 | 0x04<<16 | 0xF4<<24`, so **MAIR0 = 0xF404FF44** and **MAIR1 = 0x00000000**.

| AttrIndx | Attribute | Used for |
|---|---|---|
| 0 | `0x44` Normal, inner and outer non-cacheable | default (no IOMMU_CACHE) |
| 1 | `0xFF` Normal, WB RW-allocate | `IOMMU_CACHE` (msm `MSM_BO_CACHED_COHERENT`) |
| 2 | `0x04` Device-nGnRE | `IOMMU_MMIO` |
| 3 | `0xF4` inner NC, outer WBRWA | system-cache/LLC case, unused here |

### 3.5 Descriptor layout

PTE bits are in iopt.c:72-99. `prot_to_pte` is iopt.c:481-549, init_pte is iopt.c:339-358, and install_table is iopt.c:392-422.

- **Table descriptor** (levels 0-2): `[1:0] = 0b11`, `[47:12] = next-table PA`.
  - No APTable, XNTable or PXNTable.
  - NSTable[63] only with the `ARM_NS` quirk, which is not used.
- **Block descriptor** (L1 = 1 GB, L2 = 2 MB): `[1:0] = 0b01`.
- **Page descriptor** (L3 = 4 KB): `[1:0] = 0b11`.
- **Leaf attributes:**
  - `AttrIndx[4:2]`, from the table above.
  - `NS[5]` = 0.
  - `AP[1]` (bit 6, "UNPRIV") = 1 unless `IOMMU_PRIV`. msm sets PRIV for `MSM_BO_MAP_PRIV` (msm_gem.c:490-491, git HEAD), for example the GMU icache/dcache (a6xx_gmu.c:1293-1294).
  - `AP[2]` (bit 7, RDONLY) = 1 for read-only (`!IOMMU_WRITE`).
  - `SH[9:8]` = IS (3) if `IOMMU_CACHE`, otherwise OS (2).
  - `AF[10]` = 1 always. SCTLR.AFE = 1, so a clear AF raises an access-flag fault.
  - **`nG[11]` = 1 always** for S1.
  - `[47:12]` = output PA.
  - `DBM[51]`: only with the HD quirk (not used).
  - `XN[54:53] = 0b11` only with `IOMMU_NOEXEC`. msm GEM never passes NOEXEC (msm_gem.c:482-494), so it is 0.
  - Bit 55 = software `SW_SYNC` (non-coherent only).
- Examples (4 KB page, PA ORed into [47:12]):

  | Mapping | PTE bits |
  |---|---|
  | RW, uncached, user | `0x...E43` |
  | RW, cached-coherent, user | `0x...F47` |
  | RO | add `0x80` |
  | Privileged | remove `0x40` |
  | Device | `0x...E4B` |

- On map, a leaf is never overwritten (`-EEXIST`). Replacing a table with a block first unmaps the subtree and flushes the walk (iopt.c:360-390).

### 3.6 Final CB register values (summary)

| Reg | GPU CB0 | GMU CB1 |
|---|---|---|
| ACTLR | 0x303 (upstream) | 0x303 |
| CBA2R | 0x00000001 (\| 0<<16) | 0x00000001 (\| 1<<16 if VMID16EN) |
| CBAR | 0x0001F300 | 0x0001F300 |
| TCR2 | 0x00038010 \| ips | same |
| TCR | §3.2 table | §3.2 table |
| TTBR0 | 0 (EPD0) or table \| 0<<48 | table \| 1<<48 |
| TTBR1 | table (ASID 0) | 1<<48 |
| MAIR0/1 | 0xF404FF44 / 0 | same |
| SCTLR | 0x1167 (HUPCF + CFIE/CFRE/AFE/TRE/M/ASIDPNE); 0x11E7 with stall (CFCFG) | 0x1167 |
| S2CR | 0x00000000 (TRANS, CB0) | 0x00000001 |
| SMR | 0x84010000 | 0x84000005 |

### 3.7 Coherency of page-table writes

- **Coherent walker** (`coherent_walk`, from DT `dma-coherent` only: smmu.c:2064-2065, 811):
  - Page tables are plain cacheable memory with **no cache maintenance** (iopt.c:276, 356-357, 413-414).
  - Ordering uses barriers only: `dma_wmb()` before linking a new table (iopt.c:409), `wmb()` (DSB) after map (iopt.c:576-580), and `wmb()` before TLBI (smmu.c:264, 289-290).
  - The XNU mapping of the tables must be Normal, inner-shareable, WB, so it matches TCR SH = IS and IRGN/ORGN = WBWA.
- **Non-coherent walker:** every PTE update is followed by `dma_sync_single_for_device` (clean to PoC) on the touched entries (iopt.c:318-332, 356-357, 417-419, 464-466). New tables are `dma_map`ped (iopt.c:276-287).
- **Conflict between DTs.** Upstream DT says coherent (sm8250.dtsi:3109). Stock DT does not (kona-v21.dts:9743-9779). Log IDR0.CTTW.
  - Safe universal fallback: non-coherent TCR (SH = OS, RGN = NC) plus `DC CVAC` on each PTE write, then `DSB SY`. This is correct whether or not the walk path snoops.

---

## 4. TLB maintenance

**Generic S1 ops** (smmu.c:257-397):

- **flush_all**: `DSB`; `TLBIASID = asid` (32-bit, CB+0x610); then context sync (smmu.c:257-268).
- **flush_walk**, used when a table is freed during unmap: qcom sets `flush_walk_prefer_tlbiasid`, so this is TLBIASID + sync (smmu.c:329-342, qcom.c:323).
- **add_page**, per unmapped leaf: `DSB` (coherent case); then a 64-bit `TLBIVAL` (CB+0x620) with `(iova >> 12) | asid << 48` for each granule. There is no sync until `iotlb_sync` (smmu.c:281-307, 344-350, 1314-1330).
  - **Caution:** Linux does not mask `iova>>12` to 44 bits. For sign-extended TTBR1 addresses, bits [51:48] of `iova>>12` are ones and get ORed into the ASID field. Our driver must use `((iova >> 12) & GENMASK(43,0)) | asid<<48`, or simply use TLBIASID.
- **Map** needs **no** TLB maintenance, only `DSB` (iopt.c:576-580). Linux does nothing after `map_pages`.
- **Unmap of a leaf:** clear the PTE (plus a cache clean if non-coherent), `DSB`, `TLBIVAL` per page or one `TLBIASID`, then sync.
- **drm/msm per-process tables:** `add_page` is a no-op and every unmap ends with `iommu_flush_iotlb_all(parent)`, which is a TLBIASID(0) + sync on CB0 (msm_iommu.c:101-132, 443-483). It skips TLB flushes while the SMMU is runtime-suspended (msm_iommu.c:448-449, 464-465).

**Sync procedure** (`__arm_smmu_tlb_sync` / `qcom_smmu_tlb_sync`, smmu.c:213-234, qcom.c:58-76):

```
write32(CB+0x7f0 TLBSYNC, 0xffffffff)        // never WZR
for (delay = 1; delay < 1000000; delay *= 2):  // TLB_LOOP_TIMEOUT = 1e6 us (smmu.h:257)
    repeat 10 times (TLB_SPIN_COUNT, smmu.h:258):
        if !(read32(CB+0x7f4 TLBSTATUS) & 1): return OK
        cpu_relax
    udelay(delay)
timeout (~1 s total): "TLB sync timed out -- SMMU may be deadlocked"
```

- The global equivalent uses `sTLBGSYNC` (GR0+0x70) and `sTLBGSTATUS` (GR0+0x74, bit 0 GSACTIVE) (smmu.c:236-244). Linux uses it only in reset and for stage-2. **Avoid it**; use the per-CB sync.
- A TLBI or sync with the CX power domain off will hang the bus. Power-gate only when idle, and invalidate after power-up.

---

## 5. Fault handling

**Context fault** (level IRQ per CB):

- Read the fault state (smmu.c:412-419, qcom.c:95-109):
  - `FSR` (CB+0x58).
  - `FAR` (CB+0x60, 64-bit, faulting input address).
  - `FSYNR0` (CB+0x68).
  - `FSYNR1` (CB+0x6c).
  - `CBFRSYNRA(n)` (GR1+0x400+4n; [15:0] = SID).
  - Optionally `TTBR0` and `CONTEXTIDR`.
- If `FSR & 0xC00001FE == 0`, the IRQ is not ours (smmu.c:467-468).
- FSR bits (names from smmu.h:200-224; meanings [ARCH]):

  | Bit | Name | Meaning |
  |---|---|---|
  | TF[1] | translation fault | no valid descriptor |
  | AFF[2] | access flag fault | |
  | PF[3] | permission fault | for example a write to an AP[2] = 1 page, or an unprivileged access to a PRIV page |
  | EF[4] | external fault | bus abort on the walk or the data |
  | TLBMCF[5] | TLB match conflict | |
  | TLBLKF[6] | TLB lock fault | |
  | ASF[7] | address size fault | for example a PA ≥ 2^oas in a descriptor, or an input outside T0SZ/T1SZ ranges |
  | UUT[8] | unsupported upstream transaction | |
  | FORMAT[10:9] | | |
  | SS[30] | stalled, awaiting RESUME | only if SCTLR.CFCFG = 1 |
  | MULTI[31] | another fault occurred while FSR was non-zero | |

- adreno classifies the fault as TF = "TRANSLATION", PF = "PERMISSION", EF = "EXTERNAL" (adreno_gpu.c:287-334).
- FSYNR0 fields: `PLVL[1:0]` (table level), `WNR[4]` (1 = write; Linux uses it for READ/WRITE, smmu.c:470-471), `PNU[5]` (privileged), `IND[6]` (instruction), `NSATTR[8]`, `PTWF[10]` (fault during the table walk), `AFR[11]`, `S1CBNDX[23:16]` (smmu.h:228-236, smmu.c:443-452).
- FSYNR1 on A6xx: `[7:0]` = GPU client ID. 0 = CP, 4 = CCU, 6 = CDP prefetch, 7 = GMU, other = UCHE sub-client (a6xx_gpu.c:1614-1650). GPU scratch registers CP_SCRATCH_REG(4..7) are also dumped (a6xx_gpu.c:1639-1644).
- **Clear:** write the value read back to FSR (write-1-to-clear). Then, if `FSR.SS`, write `RESUME` (CB+0x8) = 1 to terminate or 0 to retry (smmu.c:476-481).
  - The qcom debug handler documents the alternative order for stall debugging: resume first, then clear FSR, so that HUPCF still applies to followers (qdbg.c:450-473).
- **Stall policy:** msm turns stall on (CFCFG) at creation (msm_iommu.c:789-791). On a fault it turns stall off for ≥ 500 ms (adreno_gpu.c:303-315). The generic handler terminates (smmu.c:478-481). While stalled, `RBBM_STATUS3.SMMU_STALLED_ON_FAULT` (bit 24) is set and hang-check bails (a6xx.xml:348, a6xx_gpu.c:1706-1707).
- **First-cut recommendation:** leave **CFCFG = 0** (terminate on fault, no RESUME needed) and **HUPCF = 1** (qcom.c:83-90). Faults then never wedge the GPU, and the IRQ only needs FSR cleared.

**Global fault** (SPI 672/673):

- Read `sGFSR` (GR0+0x48) and `sGFSYNR0/1/2` (0x50/54/58). `sGFSYNR1[15:0]` is the stream ID; `USF` (bit 1) means an unidentified stream when USFCFG = 1 (smmu.c:486-517).
- Clear by writing sGFSR back.
- [ARCH] Other sGFSR bits: ICF[0], SMCF[2] (stream-match conflict, for example overlapping SMRs), UCBF[3], UCIF[4], CAF[5], EF[6], PF[7], UUT[8], MULTI[31].

---

## 6. CP-driven page-table switching (per-process) and whether to skip it

**What the CP needs:**

1. **CB0 hosts the GPU domain.** Linux forces it (qcom.c:274-286). TZ/QHEE must be told, through the SCM aperture call with cb = 0, to let the CP reach CB0 (adreno_gpu.c:662-668, qcom_scm.c:919-933). CB0's registers are at GPU-space offset **0xa8000** (stock `qcom,gpu-offset`, kona-v21.dts:24350), which is 0x3d00000 + 0xa8000 = 0x3da8000 = CB0 (§1.1).
2. **Split page tables in CB0, with TCR already enabling TTBR0** (EPD0 = 0, correct T0SZ/TG0/SH0/RGN0). The kernel does this once through `set_ttbr0_cfg` when the first per-process table is created (msm_iommu.c:575-585, qcom.c:248-263). The CP never writes TCR.
3. **The packet** `CP_SMMU_TABLE_UPDATE` (opcode 0x53, adreno_pm4.xml:476-477, 2223-2243) carries 4 dwords:

   | Dword | Content |
   |---|---|
   | 0 | TTBR0_LO[31:0] |
   | 1 | TTBR0_HI[15:0] \| ASID[31:16] |
   | 2 | CONTEXTIDR ("unused, does not apply to aarch64") |
   | 3 | CONTEXTBANK |

   msm emits it as (a6xx_gpu.c:110-220):

   ```
   CP_SET_PROTECTED_MODE(0)                       // a6xx only, to write the SMMU
   PKT4 RBBM_PERFCTR_SRAM_INIT_CMD = 1            // unless sysprof
   CP_SMMU_TABLE_UPDATE: ttbr_lo, ttbr_hi|asid<<16, 0, 0  // ASID = 0, CONTEXTBANK = 0
   CP_MEM_WRITE memptrs->ttbr0 = ttbr, seqno      // for debug / preemption
   CP_EVENT_WRITE CACHE_INVALIDATE                // UCHE flush
   CP_WAIT_REG_MEM RBBM_PERFCTR_SRAM_INIT_STATUS == 1
   CP_SET_PROTECTED_MODE(1)
   ```

   The kernel issues no CPU-side TLBI on a switch. Since every process uses ASID 0, the CP firmware must invalidate and sync CB0's TLB itself as part of `CP_SMMU_TABLE_UPDATE` [INFER].
4. **CP register protection.** The SMMU window must be write-protected against user command streams. Upstream relies on the A650 protect list's final "infinite range" entry `A6XX_PROTECT_NORDWR(0x1f8c0, 0)`, which covers all dword offsets ≥ 0x1f8c0, including SMMU dword 0x28000 (a6xx_catalog.c:546-587). Stock KGSL uses `qcom,protect = <0xa0000 0xc000>`, which covers the global space plus CB0-CB3 [INFER]. A650 has HW APRIV, so CP/kernel fetches are privileged (a6xx_catalog.c:973-974).
5. **Preemption** (not needed for a first cut) also records the SMMU state through `CP_SET_PSEUDO_REG SMMU_INFO` and `CP_CONTEXT_SWITCH_SMMU_INFO` (a6xx_gpu.c:314, a6xx_preempt.c:210, 306).

**Can a first cut skip per-process page tables?** Yes. There are two options:

- **Option A: one shared table, TTBR0 only.** TCR = `0x0080351x` (EPD1 = 1). Put kernel and user GPU VAs in the same table below 2^ias. Never emit `CP_SMMU_TABLE_UPDATE`, and the SCM aperture call is optional. This is the simplest, and CB0 looks exactly like the GMU CB.
- **Option B: the Linux layout with a single TTBR0.** Kernel/global buffers go in TTBR1 (sign-extended high VAs); one shared user table goes in TTBR0 (TCR `0xB519351x`/`0xB510351x`). It is equally simple, and the upgrade path to per-process tables needs only `CP_SMMU_TABLE_UPDATE` plus the aperture SCM call.

**Consequences of a single address space:**

1. **No isolation between GPU clients.** Any submitted command stream or shader can read and write every GPU-mapped buffer of every process, including the ringbuffer, memptrs, fences, other apps' render targets and textures. That is a security and stability problem for anything multi-tenant.
   - Partial mitigation: map kernel-only buffers privileged (AP[1] = 0, PRIV). With HW APRIV, only CP-internal fetches are privileged, so user shaders and IBs (unprivileged) get permission faults on them. This requires S2CR.PRIVCFG = 0 (default, use incoming).
2. **One shared VA allocator**, so userspace (Mesa/turnip-style drivers) cannot choose VAs freely (no VM_BIND per process). Fragmentation or exhaustion is shared, and a fault in one context can only be attributed by IOVA.
3. **Correctness is otherwise fine.** No TLB issues arise (one ASID, invalidate on unmap), and no CP aperture or protected-mode juggling is needed.
4. If you later enable per-process tables, keep ASID 0 everywhere (upstream behaviour). Otherwise the CP's TLB maintenance assumptions and `iommu_flush_iotlb_all` (TLBIASID with the CB's ASID) diverge (msm_iommu.c:613-619).

---

## Key risks (short list)

1. **Hypervisor-mediated global space.**
   - Never use WZR stores (the GPU's S2CR value is 0!), LDP/STP or writeback addressing.
   - Read back SMR/S2CR. Writes may be filtered (smmu.c:44-51, qcom.c:446-458).
2. **Do not run a Linux-style global reset.** Upstream does one for this instance (no handoff for the Adreno impl), but it would wipe any firmware/secure SMEs. Keep foreign SMR/S2CR/CB state.
3. **GPU must be CB0.** If firmware already uses CB0, stop. Do not steal it.
4. **Power domain.** Registers and TLBs live in GPU_CX_GDSC. Any access while it is off hangs the bus, and state may be lost, so reprogram after power-up (smmu.c:2289-2301).
5. **Coherency mismatch** between upstream (`dma-coherent`) and stock DT (none). The non-coherent TCR plus cache clean is the safe fallback.
6. **The TLBIVA ASID corruption pattern** in Linux for TTBR1 addresses: mask the VA.
7. **Unverified values to read on hardware:** ID2.IAS/UBS (oas/ias), NUMCB, NUMSMRG, the live sCR0 (CLIENTPD, USFCFG, EXIDENABLE, VMID16EN), ID7 (CACHE_LOCK applicability), and the bootloader's SMR/S2CR/CB contents.
