/*
 * dispatch.c - host-test stand-in for SweetAdrenoUserClient
 * (kernel/SweetAdreno.cpp). Same validation and the same calls into the
 * driver core, minus IOKit: memory comes from the model's DMA allocator
 * instead of IOBufferMemoryDescriptor, and the "task mapping" is the host
 * pointer. Lets libsweetgpu + gputest run end-to-end on Linux.
 */
#include <errno.h>
#include <string.h>
#include "model.h"
#include "../kernel/sweet_gpu.h"

struct sgpu host_g;

struct hmem_obj {
	bool            used;
	uint32_t        id;
	struct sgpu_dma dma;
	uint64_t        gpuaddr, size;
	uint32_t        free_fence;
	bool            dying;
};
static struct hmem_obj objs[256];
static uint64_t ctx_mask;

static void
reap(void)
{
	uint32_t retired = sgpu_cp_ring_fence_retired(&host_g);
	for (int i = 0; i < 256; i++) {
		struct hmem_obj *o = &objs[i];
		if (o->used && o->dying && sgpu_ts_ge(retired, o->free_fence)) {
			sgpu_smmu_unmap(&host_g, SGPU_DOM_GPU, o->gpuaddr, o->size);
			sgpu_va_free(&host_g, o->gpuaddr, o->size);
			sgpu_dma_free(&o->dma);
			memset(o, 0, sizeof(*o));
		}
	}
}

static bool
ib_in_client_memory(uint64_t a, uint64_t bytes)
{
	for (int i = 0; i < 256; i++) {
		struct hmem_obj *o = &objs[i];
		if (o->used && !o->dying && a >= o->gpuaddr && a + bytes <= o->gpuaddr + o->size) {
			return true;
		}
	}
	return false;
}

void
host_dispatch_reset(void)
{
	memset(objs, 0, sizeof(objs));
	ctx_mask = 0;
}

int
sgpu_host_dispatch(uint32_t sel, const void *in, size_t inlen, void *out, size_t *outlen)
{
	struct sgpu *g = &host_g;

	if (sel != SGPU_SEL_GET_STATUS && sel != SGPU_SEL_GET_INFO &&
	    g->stage_reached < SGPU_STAGE_USER) {
		return -ENODEV;
	}
	switch (sel) {
	case SGPU_SEL_GET_INFO:
		if (*outlen != sizeof(struct sgpu_info)) return -EINVAL;
		sgpu_fill_info(g, out);
		return 0;
	case SGPU_SEL_GET_STATUS:
		if (*outlen != sizeof(struct sgpu_status)) return -EINVAL;
		memset(out, 0, sizeof(struct sgpu_status));
		sgpu_cp_status(g, out);
		return 0;
	case SGPU_SEL_CTX_CREATE: {
		struct sgpu_ctx_create *c = out;
		if (inlen != sizeof(*c)) return -EINVAL;
		memcpy(c, in, sizeof(*c));
		int err = sgpu_cp_ctx_alloc(g, &c->ctx_id);
		if (!err) ctx_mask |= 1ull << c->ctx_id;
		return err;
	}
	case SGPU_SEL_CTX_DESTROY: {
		const struct sgpu_ctx_destroy *c = in;
		if (c->ctx_id >= SGPU_MAX_CTX || !(ctx_mask & (1ull << c->ctx_id))) return -EINVAL;
		ctx_mask &= ~(1ull << c->ctx_id);
		sgpu_cp_ctx_free(g, c->ctx_id);
		return 0;
	}
	case SGPU_SEL_MEM_ALLOC: {
		struct sgpu_mem_alloc req;
		memcpy(&req, in, sizeof(req));
		uint64_t size = (req.size + 0x3fff) & ~0x3fffull;
		uint64_t align = req.align_log2 ? 1ull << req.align_log2 : 0x4000;
		if (size == 0 || size > (1ull << 32) || req.align_log2 > 32) return -EINVAL;
		int slot = -1;
		for (int i = 0; i < 256; i++) if (!objs[i].used) { slot = i; break; }
		if (slot < 0) return -ENOMEM;
		struct hmem_obj *o = &objs[slot];
		if (sgpu_dma_alloc(size, 0x4000, SGPU_DMA_WC, &o->dma)) return -ENOMEM;
		uint32_t prot = SGPU_PROT_READ;
		if (!(req.flags & SGPU_MEM_GPU_READONLY)) prot |= SGPU_PROT_WRITE;
		if (req.flags & SGPU_MEM_GPU_EXEC) prot |= SGPU_PROT_EXEC;
		if (req.flags & SGPU_MEM_CACHED) prot |= SGPU_PROT_CACHED;
		int err = sgpu_va_alloc(g, false, size, align, &o->gpuaddr);
		if (!err) {
			err = sgpu_smmu_map(g, SGPU_DOM_GPU, o->gpuaddr, o->dma.pa, size, prot);
			if (err) sgpu_va_free(g, o->gpuaddr, size);
		}
		if (err) {
			sgpu_dma_free(&o->dma);
			return err;
		}
		sgpu_smmu_tlb_sync(g, SGPU_DOM_GPU);
		o->used = true;
		o->id = 1;              /* lowest free id, like the kernel */
		for (bool clash = true; clash; ) {
			clash = false;
			for (int i = 0; i < 256; i++) {
				if (&objs[i] != o && objs[i].used && objs[i].id == o->id) {
					o->id++;
					clash = true;
				}
			}
		}
		o->size = size;
		req.id = o->id;
		req.gpuaddr = o->gpuaddr;
		req.cpu_addr = (req.flags & SGPU_MEM_NO_CPU_MAP) ? 0 : (uint64_t)(uintptr_t)o->dma.va;
		req.alloc_size = size;
		memcpy(out, &req, sizeof(req));
		return 0;
	}
	case SGPU_SEL_MEM_FREE: {
		const struct sgpu_mem_free *f = in;
		for (int i = 0; i < 256; i++) {
			if (objs[i].used && !objs[i].dying && objs[i].id == f->id) {
				objs[i].dying = true;
				objs[i].free_fence = sgpu_cp_ring_fence_submitted(g);
				reap();
				return 0;
			}
		}
		return -ENOENT;
	}
	case SGPU_SEL_SYNC_CACHE:
		return 0;
	case SGPU_SEL_SUBMIT: {
		struct sgpu_submit *s = out;
		memcpy(s, in, sizeof(*s));
		if (s->ctx_id >= SGPU_MAX_CTX || !(ctx_mask & (1ull << s->ctx_id)) ||
		    s->num_ibs > SGPU_MAX_IBS) return -EINVAL;
		for (uint32_t i = 0; i < s->num_ibs; i++) {
			uint64_t bytes = (uint64_t)s->ibs[i].size_dwords * 4;
			if (bytes == 0 || bytes > (1u << 22) || (s->ibs[i].gpuaddr & 3) ||
			    !ib_in_client_memory(s->ibs[i].gpuaddr, bytes)) return -EINVAL;
		}
		int err = sgpu_cp_submit(g, s->ctx_id, s->ibs, s->num_ibs, s->flags, &s->timestamp);
		reap();
		return err;
	}
	case SGPU_SEL_WAIT_TS: {
		const struct sgpu_wait_ts *w = in;
		if (w->ctx_id >= SGPU_MAX_CTX || !(ctx_mask & (1ull << w->ctx_id))) return -EINVAL;
		uint64_t start = sgpu_now_us();
		for (;;) {
			if (sgpu_ts_ge(sgpu_cp_retired(g, w->ctx_id), w->timestamp)) {
				reap();
				return 0;
			}
			if (sgpu_cp_hang_check(g)) return -EIO;
			if (sgpu_now_us() - start >= (uint64_t)w->timeout_ms * 1000) return -ETIMEDOUT;
			sgpu_udelay(100);
		}
	}
	case SGPU_SEL_READ_TS: {
		struct sgpu_read_ts *r = out;
		memcpy(r, in, sizeof(*r));
		if (r->ctx_id >= SGPU_MAX_CTX || !(ctx_mask & (1ull << r->ctx_id))) return -EINVAL;
		r->retired = sgpu_cp_retired(g, r->ctx_id);
		r->queued = sgpu_cp_queued(g, r->ctx_id);
		return 0;
	}
	case SGPU_SEL_GPU_TIME: {
		struct sgpu_gpu_time *t = out;
		t->gpu_ticks = sgpu_cp_always_on(g);
		t->cpu_ticks = sgpu_now_us();
		return 0;
	}
	case SGPU_SEL_IMPORT_SURFACE:
		return -EOPNOTSUPP;
	}
	return -EINVAL;
}
