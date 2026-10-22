/*
 * sweet_gpu_mem.c - GPU virtual address allocation and kernel buffers.
 *
 * One GPU address space (the SMMU context bank of the GPU) shared by the
 * kernel and every client in this first version: kernel buffers (ring,
 * memptrs, SQE microcode, self-test scratch) live in [16 MiB, 256 MiB),
 * user buffers above 4 GiB. Per-process page tables (CP_SMMU_TABLE_UPDATE)
 * can come later without changing the UAPI: user addresses stay in the
 * same window.
 *
 * The allocator is a sorted array of free ranges with first-fit; GPU
 * allocations are few and large, so O(n) is fine. All sizes are multiples
 * of the 16 KiB CPU page (also a multiple of the 4 KiB SMMU granule).
 * Callers serialise (SweetAdreno.cpp holds its lock around every call).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(SGPU_HOST)
#include "sweet_gpu.h"

#define VA_MAX_RANGES   1024
#define VA_PAGE         0x4000ull

struct va_range {
	uint64_t base, size;
};

struct va_space {
	struct va_range r[VA_MAX_RANGES];
	uint32_t n;
};

static struct va_space va_kernel, va_user;

static void
va_reset(struct va_space *s, uint64_t base, uint64_t size)
{
	s->n = 1;
	s->r[0].base = base;
	s->r[0].size = size;
}

int
sgpu_va_init(struct sgpu *g)
{
	(void)g;
	va_reset(&va_kernel, SGPU_VA_KERNEL_BASE, SGPU_VA_KERNEL_SIZE);
	va_reset(&va_user, SGPU_VA_USER_BASE, SGPU_VA_USER_SIZE);
	return 0;
}

int
sgpu_va_alloc(struct sgpu *g, bool kernel, uint64_t size, uint64_t align, uint64_t *iova)
{
	struct va_space *s = kernel ? &va_kernel : &va_user;

	(void)g;
	if (size == 0) {
		return -22;
	}
	size = (size + VA_PAGE - 1) & ~(VA_PAGE - 1);
	if (align < VA_PAGE) {
		align = VA_PAGE;
	}
	if (align & (align - 1)) {
		return -22;
	}
	for (uint32_t i = 0; i < s->n; i++) {
		uint64_t lo = (s->r[i].base + align - 1) & ~(align - 1);
		uint64_t end = s->r[i].base + s->r[i].size;
		if (lo < s->r[i].base || lo + size > end || lo + size < lo) {
			continue;
		}
		/* carve [lo, lo+size) out of range i */
		uint64_t head = lo - s->r[i].base, tail = end - (lo + size);
		if (head && tail) {
			if (s->n == VA_MAX_RANGES) {
				return -12;
			}
			for (uint32_t j = s->n; j > i + 1; j--) {
				s->r[j] = s->r[j - 1];
			}
			s->n++;
			s->r[i].size = head;
			s->r[i + 1].base = lo + size;
			s->r[i + 1].size = tail;
		} else if (head) {
			s->r[i].size = head;
		} else if (tail) {
			s->r[i].base = lo + size;
			s->r[i].size = tail;
		} else {
			for (uint32_t j = i; j + 1 < s->n; j++) {
				s->r[j] = s->r[j + 1];
			}
			s->n--;
		}
		*iova = lo;
		return 0;
	}
	return -12;
}

void
sgpu_va_free(struct sgpu *g, uint64_t iova, uint64_t size)
{
	struct va_space *s = iova < SGPU_VA_USER_BASE ? &va_kernel : &va_user;
	uint32_t i;

	(void)g;
	size = (size + VA_PAGE - 1) & ~(VA_PAGE - 1);
	for (i = 0; i < s->n && s->r[i].base < iova; i++) {
	}
	/* merge with predecessor and/or successor, else insert at i */
	bool prev = i > 0 && s->r[i - 1].base + s->r[i - 1].size == iova;
	bool next = i < s->n && iova + size == s->r[i].base;
	if (prev && next) {
		s->r[i - 1].size += size + s->r[i].size;
		for (uint32_t j = i; j + 1 < s->n; j++) {
			s->r[j] = s->r[j + 1];
		}
		s->n--;
	} else if (prev) {
		s->r[i - 1].size += size;
	} else if (next) {
		s->r[i].base = iova;
		s->r[i].size += size;
	} else {
		if (s->n == VA_MAX_RANGES) {
			sgpu_log("va: free list full, leaking %#llx+%#llx",
			    (unsigned long long)iova, (unsigned long long)size);
			return;
		}
		for (uint32_t j = s->n; j > i; j--) {
			s->r[j] = s->r[j - 1];
		}
		s->r[i].base = iova;
		s->r[i].size = size;
		s->n++;
	}
}

int
sgpu_bo_alloc(struct sgpu *g, size_t size, uint32_t prot, struct sgpu_bo *bo)
{
	int err;

	err = sgpu_dma_alloc(size, VA_PAGE, SGPU_DMA_WC, &bo->dma);
	if (err) {
		return err > 0 ? -err : err;
	}
	err = sgpu_va_alloc(g, true, bo->dma.size, VA_PAGE, &bo->iova);
	if (err) {
		sgpu_dma_free(&bo->dma);
		return err;
	}
	err = sgpu_smmu_map(g, SGPU_DOM_GPU, bo->iova, bo->dma.pa, bo->dma.size, prot);
	if (err) {
		sgpu_va_free(g, bo->iova, bo->dma.size);
		sgpu_dma_free(&bo->dma);
		return err;
	}
	return 0;
}

void
sgpu_bo_free(struct sgpu *g, struct sgpu_bo *bo)
{
	if (bo->dma.va == NULL) {
		return;
	}
	sgpu_smmu_unmap(g, SGPU_DOM_GPU, bo->iova, bo->dma.size);
	sgpu_va_free(g, bo->iova, bo->dma.size);
	sgpu_dma_free(&bo->dma);
	bo->iova = 0;
}
#endif
