/*
 * sweet_gpu_os_dma.cpp - physically contiguous DMA memory for the SWEET
 * Adreno driver, built on IOBufferMemoryDescriptor.
 *
 * Why IOKit and not kmem_alloc_contig: the GPU, the GMU and the SMMU
 * walker need buffers whose CPU view is Normal non-cacheable (Linux:
 * MSM_BO_WC). IOKit is the supported way to get such a kernel mapping of
 * RAM (createMappingInTask(kernel_task, kIOMapWriteCombineCache)); the
 * pmap enforces consistent attributes for managed pages there, which a
 * raw ml_io_map of RAM would bypass.
 *
 * Aliasing rule: the descriptor's own (cacheable) kernel mapping is
 * zeroed, cleaned+invalidated to PoC once, and then never touched; all
 * CPU access goes through the WC map. No dirty line can therefore be
 * written back over data the device produced.
 *
 * Physical address limit: SMMU output size on MMU-500 SM8250 is at least
 * 36 bits (read from ID2 at stage 3); RAM on apollo ends below 0x2_8000_0000,
 * so a 36-bit mask never excludes usable memory.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>

extern "C" {
#include "../../../bsd/dev/sweet_gpu.h"
void flush_dcache64(addr64_t addr, unsigned count, int phys);
}

#define SGPU_DMA_PHYS_MASK      0x0000000ffffff000ULL   /* 36-bit PA */

extern "C" int
sgpu_dma_alloc(size_t size, size_t align, uint32_t flags, struct sgpu_dma *out)
{
	bzero(out, sizeof(*out));
	size = round_page(size);
	if (align < PAGE_SIZE) {
		align = PAGE_SIZE;
	}
	if (align & (align - 1)) {
		return EINVAL;
	}
	mach_vm_address_t mask = SGPU_DMA_PHYS_MASK & ~((mach_vm_address_t)align - 1);

	IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
		kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, size, mask);
	if (md == NULL) {
		return ENOMEM;
	}
	if (md->prepare() != kIOReturnSuccess) {
		md->release();
		return ENOMEM;
	}
	IOByteCount seglen = 0;
	addr64_t pa = md->getPhysicalSegment(0, &seglen, kIOMemoryMapperNone);
	if (pa == 0 || seglen < size) {
		/* kIOMemoryPhysicallyContiguous should make this impossible */
		md->complete();
		md->release();
		return EFAULT;
	}
	void *cva = md->getBytesNoCopy();
	bzero(cva, size);
	/* push the zeroes to DRAM and drop the lines (clean + invalidate) */
	flush_dcache64((addr64_t)(uintptr_t)cva, (unsigned)size, FALSE);

	out->pa = pa;
	out->size = size;
	out->os_md = md;
	if (flags & SGPU_DMA_WC) {
		IOMemoryMap *map = md->createMappingInTask(kernel_task, 0,
		    kIOMapAnywhere | kIOMapWriteCombineCache);
		if (map == NULL) {
			md->complete();
			md->release();
			bzero(out, sizeof(*out));
			return ENOMEM;
		}
		out->os_map = map;
		out->va = (void *)map->getVirtualAddress();
	} else {
		out->va = cva;
	}
	return 0;
}

extern "C" void
sgpu_dma_free(struct sgpu_dma *dma)
{
	if (dma->os_map != NULL) {
		((IOMemoryMap *)dma->os_map)->release();
	}
	if (dma->os_md != NULL) {
		IOBufferMemoryDescriptor *md = (IOBufferMemoryDescriptor *)dma->os_md;
		md->complete();
		md->release();
	}
	bzero(dma, sizeof(*dma));
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
