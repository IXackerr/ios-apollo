/*
 * model.h - SM8250 / Adreno 650 behavioural model for the host test.
 *
 * Not a GPU simulator: it models exactly the things the SWEET driver
 * depends on and checks the driver against the spec while doing so:
 *   - clocks/GDSCs (enable bits, status bits, CX/GX power gating of
 *     register access: touching GMU/SMMU with CX off or GPU with GX off
 *     is an error, like a bus hang on hardware),
 *   - the MMU-500 (SMR/S2CR stream matching, context banks, a real LPAE
 *     stage-1 walker with AP/permission checks and FSR/FAR reporting),
 *   - the GMU (RSC handshakes, firmware boot, an HFI v2 peer that parses
 *     and validates every message and acks it, OOB doorbells, GX power),
 *   - the CP (PM4 type-4/type-7 parser with parity checks, ringbuffer +
 *     IB1/IB2 fetch through the SMMU, privilege per APRIV, CP_MEM_WRITE,
 *     CP_EVENT_WRITE CACHE_FLUSH_TS, CP_REG_TO_MEM, secure mode),
 *   - TrustZone (SCM is-available, PAS init/mem-setup/auth for id 13).
 * Every violation increments model_errors and prints why.
 */
#ifndef SGPU_MODEL_H
#define SGPU_MODEL_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

extern int model_errors;
extern uint64_t model_now_us;
extern uint64_t model_accesses;         /* register accesses since reset */

struct model_opts {
	bool    zap_present;            /* ADT has zap firmware */
	bool    tz_protects_secvid;     /* CPU write to SECVID_TRUST_CNTL = violation */
	bool    cb0_live;               /* firmware left CB0 enabled */
	bool    fw_sme_sid2;            /* firmware SME for SID 2/0x400 -> live CB3 */
	bool    clientpd;               /* SMMU globally bypassing at boot */
	bool    hv_filters_s2cr;        /* hypervisor drops S2CR writes */
	uint32_t speed_bin;
	bool    permissive_pm4;         /* skip unmodelled opcodes (real turnip streams) */
};

void  model_reset(const struct model_opts *o);
void  model_set_stall(bool stall);      /* CP stops consuming the ring */
void  model_tick(void);

/* memory */
void *host_pa_to_va(uint64_t pa, size_t len);

/* register readback for checks */
uint32_t model_gpu_reg(uint32_t dword);
uint32_t model_gmu_reg(uint32_t byte_off);
uint32_t model_smmu_reg(uint32_t byte_off);
uint32_t model_rscc_reg(uint32_t byte_off);
uint32_t model_pdc_reg(uint32_t byte_off);

/* HFI message ids seen, in order */
extern uint32_t model_hfi_log[64];
extern int model_hfi_count;
extern uint32_t model_cp_packets;
extern uint32_t model_cp_skipped;    /* unmodelled packets skipped (permissive) */

/* reference PM4 encoders (independent of the driver) */
uint32_t ref_pkt7(uint32_t op, uint32_t cnt);
uint32_t ref_pkt4(uint32_t reg, uint32_t cnt);

/* firmware images synthesised for the ADT */
extern uint8_t *fw_sqe, *fw_gmu, *fw_zap;
extern uint32_t fw_sqe_len, fw_gmu_len, fw_zap_len;

/* boot-args / ADT used by os_host.c */
void host_set_bootarg(const char *name, uint32_t v);
void host_clear_bootargs(void);
#endif
