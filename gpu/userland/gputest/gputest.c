/*
 * gputest - first user-space test of the SWEET Adreno driver on the phone.
 *
 * No Mesa, no shaders: it writes raw PM4 into its own GPU buffer and
 * submits it, which proves the whole path user -> IOKit -> CP -> SMMU ->
 * memory -> timestamp works before turnip is involved.
 *
 *   gputest            info + basic test (CP_MEM_WRITE from an IB)
 *   gputest -n 1000    + 1000-submit throughput/latency run
 *   gputest -f         + deliberate GPU page fault (address 0), expects
 *                        the kernel to report it in status.faults
 *   gputest -c         + cache coherence of the caller's pages, mapped for
 *                        the GPU plain and SGPU_MEM_CACHED
 *
 * Build on the host (iOS SDK) with tools in docs/GPU-DRIVER.md; the host
 * test (hosttest/) links this file too, with SGPU_GPUTEST_NO_MAIN.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sweetgpu.h"

static uint32_t
parity(uint32_t v)
{
	return (0x9669u >> (0xf & (v ^ v >> 4 ^ v >> 8 ^ v >> 12 ^ v >> 16 ^ v >> 20 ^
	       v >> 24 ^ v >> 28))) & 1u;
}

static uint32_t
pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | cnt | parity(cnt) << 15 | op << 16 | parity(op) << 23;
}

#define CP_NOP          0x10
#define CP_WAIT_MEM_WRITES 0x12
#define CP_WAIT_FOR_ME  0x13
#define CP_MEM_WRITE    0x3d
#define CP_MEMCPY       0x75

#define CHECK(x) do { int _e = (x); if (_e) { \
	fprintf(stderr, "gputest: %s failed: %d (%s)\n", #x, _e, strerror(-_e)); \
	goto out; } } while (0)

static void
print_status(sgpu_dev *dev)
{
	struct sgpu_status st;
	if (sgpu_get_status(dev, &st) == 0) {
		printf("status: stage %u/%u err %d faults %u hangs %u RBBM %#x rptr %u wptr %u "
		    "gmu %#x\n  msg: %s\n", st.stage_reached, st.stage_requested, st.last_error,
		    st.faults, st.hangs, st.rbbm_status, st.cp_rb_rptr, st.cp_rb_wptr, st.gmu_state,
		    st.message[0] ? st.message : "-");
	}
}

/*
 * Whether the GPU sees the CPU caches through pages the caller imports
 * with flags: the CPU writes a pattern into A and leaves it in its cache,
 * reads B into its cache, and the CP copies A to B. B as the CPU reads it
 * then, and again after an invalidate, tells both directions apart:
 * "stale" words are old lines the GPU's writes left in the CPU cache,
 * "missed" ones CPU writes the GPU's reads did not see.
 */
#define COHERENCE_BYTES (256 * 1024)
#define COHERENCE_ROUNDS 16

static int
coherence(sgpu_dev *dev, uint32_t ctx, struct sgpu_mem_alloc *ib, uint32_t flags,
    const char *name)
{
	uint32_t words = COHERENCE_BYTES / 4, stale = 0, missed = 0, ts;
	uint32_t *a = NULL, *b = NULL;
	struct sgpu_import_user ia = { 0 }, ib_ = { 0 };
	int err = -ENOMEM;

	if (posix_memalign((void **)&a, 16384, COHERENCE_BYTES) ||
	    posix_memalign((void **)&b, 16384, COHERENCE_BYTES)) {
		goto out;
	}
	memset(a, 0, COHERENCE_BYTES);
	memset(b, 0, COHERENCE_BYTES);
	ia = (struct sgpu_import_user){ .addr = (uintptr_t)a, .size = COHERENCE_BYTES,
	    .flags = flags };
	ib_ = (struct sgpu_import_user){ .addr = (uintptr_t)b, .size = COHERENCE_BYTES,
	    .flags = flags };
	if ((err = sgpu_import_user(dev, &ia)) || (err = sgpu_import_user(dev, &ib_))) {
		fprintf(stderr, "gputest: coherence %s: import failed: %d\n", name, err);
		goto out;
	}
	volatile uint32_t *cmd = (volatile uint32_t *)(uintptr_t)ib->cpu_addr + 0x200;
	uint32_t k = 0;
	cmd[k++] = pkt7(CP_MEMCPY, 5);
	cmd[k++] = words;
	cmd[k++] = (uint32_t)ia.gpuaddr;
	cmd[k++] = (uint32_t)(ia.gpuaddr >> 32);
	cmd[k++] = (uint32_t)ib_.gpuaddr;
	cmd[k++] = (uint32_t)(ib_.gpuaddr >> 32);
	cmd[k++] = pkt7(CP_WAIT_MEM_WRITES, 0);
	cmd[k++] = pkt7(CP_WAIT_FOR_ME, 0);
	__sync_synchronize();
	struct sgpu_ib copy = { ib->gpuaddr + 0x800, k, 0 };

	for (uint32_t round = 0; round < COHERENCE_ROUNDS; round++) {
		uint32_t seed = 0x9e3779b9u * (round + 1), sum = 0;
		for (uint32_t i = 0; i < words; i++) {
			a[i] = seed ^ i;
		}
		for (uint32_t i = 0; i < words; i++) {
			sum += b[i];
		}
		__asm__ volatile("dsb sy" ::: "memory");
		if ((err = sgpu_submit(dev, ctx, &copy, 1, 0, &ts)) ||
		    (err = sgpu_wait_ts(dev, ctx, ts, 2000))) {
			fprintf(stderr, "gputest: coherence %s: copy failed: %d (sum %u)\n", name, err,
			    sum);
			goto out;
		}
		for (uint32_t i = 0; i < words; i++) {
			stale += b[i] != (seed ^ i);
		}
		sgpu_mem_sync(dev, ib_.id, SGPU_CACHE_INVALIDATE, 0, 0);
		for (uint32_t i = 0; i < words; i++) {
			missed += b[i] != (seed ^ i);
		}
	}
	printf("coherence %s: %u rounds of %u words: %u stale in the CPU cache, %u missed by "
	    "the GPU -> %s\n", name, COHERENCE_ROUNDS, words, stale, missed,
	    stale || missed ? "NOT coherent" : "coherent");
	err = 0;
out:
	if (ia.id) {
		sgpu_mem_free(dev, ia.id);
	}
	if (ib_.id) {
		sgpu_mem_free(dev, ib_.id);
	}
	free(a);
	free(b);
	return err;
}

int
gputest_main(int argc, char **argv)
{
	sgpu_dev *dev = NULL;
	struct sgpu_info info;
	struct sgpu_mem_alloc mem = { 0 };
	uint32_t ctx = 0, ts = 0, n = 0;
	int fault = 0, coherent = 0, rc = 1;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-n") && i + 1 < argc) {
			n = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "-f")) {
			fault = 1;
		} else if (!strcmp(argv[i], "-c")) {
			coherent = 1;
		}
	}
	CHECK(sgpu_open(&dev));
	CHECK(sgpu_get_info(dev, &info));
	printf("Adreno %u chip %#x, GMEM %u KiB, VA %#llx+%#llx, UBWC %u hbb %u, %s, max %u MHz\n",
	    info.gpu_id, info.chip_id, info.gmem_size / 1024, (unsigned long long)info.va_start,
	    (unsigned long long)info.va_size, info.ubwc_mode, info.highest_bank_bit,
	    (info.flags & SGPU_INFO_IO_COHERENT) ? "io-coherent" : "non-coherent",
	    info.max_freq_hz / 1000000);
	print_status(dev);

	CHECK(sgpu_ctx_create(dev, 0, 0, &ctx));
	mem.size = 64 * 1024;
	mem.flags = SGPU_MEM_GPU_EXEC;
	CHECK(sgpu_mem_alloc(dev, &mem));
	volatile uint32_t *cpu = (volatile uint32_t *)(uintptr_t)mem.cpu_addr;
	printf("buffer id %u gpu %#llx cpu %p\n", mem.id, (unsigned long long)mem.gpuaddr,
	    (void *)cpu);

	/* IB at +0: write 4 words to +0x1000 */
	uint64_t dst = mem.gpuaddr + 0x1000;
	uint32_t k = 0;
	cpu[k++] = pkt7(CP_MEM_WRITE, 6);
	cpu[k++] = (uint32_t)dst;
	cpu[k++] = (uint32_t)(dst >> 32);
	cpu[k++] = 0x11111111;
	cpu[k++] = 0x22222222;
	cpu[k++] = 0x33333333;
	cpu[k++] = 0x44444444;
	cpu[k++] = pkt7(CP_NOP, 0);
	cpu[0x400] = cpu[0x401] = cpu[0x402] = cpu[0x403] = 0;
	__sync_synchronize();

	struct sgpu_ib ib = { mem.gpuaddr, k, 0 };
	CHECK(sgpu_submit(dev, ctx, &ib, 1, 0, &ts));
	CHECK(sgpu_wait_ts(dev, ctx, ts, 1000));
	if (cpu[0x400] != 0x11111111 || cpu[0x403] != 0x44444444) {
		fprintf(stderr, "gputest: GPU write missing: %#x %#x %#x %#x\n", cpu[0x400],
		    cpu[0x401], cpu[0x402], cpu[0x403]);
		print_status(dev);
		goto out;
	}
	printf("basic: OK (timestamp %u)\n", ts);

	if (n) {
		uint64_t g0, c0, g1, c1;
		CHECK(sgpu_gpu_time(dev, &g0, &c0));
		for (uint32_t i = 0; i < n; i++) {
			CHECK(sgpu_submit(dev, ctx, &ib, 1, 0, &ts));
		}
		CHECK(sgpu_wait_ts(dev, ctx, ts, 5000));
		CHECK(sgpu_gpu_time(dev, &g1, &c1));
		printf("throughput: %u submits, GPU %llu ticks (%.2f us/submit)\n", n,
		    (unsigned long long)(g1 - g0), (double)(g1 - g0) / 19.2 / n);
	}

	if (coherent) {
		CHECK(coherence(dev, ctx, &mem, 0, "plain"));
		CHECK(coherence(dev, ctx, &mem, SGPU_MEM_CACHED, "cached"));
	}

	if (fault) {
		struct sgpu_status before, after;
		CHECK(sgpu_get_status(dev, &before));
		k = 0x100;
		cpu[k++] = pkt7(CP_MEM_WRITE, 3);
		cpu[k++] = 0;           /* GPU VA 0: never mapped */
		cpu[k++] = 0;
		cpu[k++] = 0xbad;
		__sync_synchronize();
		struct sgpu_ib fib = { mem.gpuaddr + 0x400, 4, 0 };
		CHECK(sgpu_submit(dev, ctx, &fib, 1, 0, &ts));
		int werr = sgpu_wait_ts(dev, ctx, ts, 2000);
		CHECK(sgpu_get_status(dev, &after));
		printf("fault test: wait %d, faults %u -> %u\n", werr, before.faults, after.faults);
		if (after.faults == before.faults) {
			fprintf(stderr, "gputest: fault not reported\n");
			goto out;
		}
	}
	rc = 0;
out:
	if (dev) {
		if (mem.id) {
			sgpu_mem_free(dev, mem.id);
		}
		if (ctx) {
			sgpu_ctx_destroy(dev, ctx);
		}
		if (rc) {
			print_status(dev);
		}
		sgpu_close(dev);
	}
	return rc;
}

#ifndef SGPU_GPUTEST_NO_MAIN
int
main(int argc, char **argv)
{
	return gputest_main(argc, argv);
}
#endif
