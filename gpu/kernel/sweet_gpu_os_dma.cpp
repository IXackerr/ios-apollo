/*
 * sweet_gpu_os_dma.cpp - physically contiguous DMA memory for the SWEET
 * Adreno driver, built on IOBufferMemoryDescriptor.
 *
 * The GPU, the GMU and the SMMU walker need buffers whose CPU view is
 * Normal non-cacheable (Linux: MSM_BO_WC). An IOMemoryMap with
 * kIOMapWriteCombineCache is not reliable for wired kernel memory: the
 * memory entry of a kernel buffer is not a VM object, so the cache-mode
 * change it asks for can fail and leave the map cacheable (the GMU then
 * faulted on page tables the CPU had already written). The pages are
 * therefore mapped a second time, write-combined, with io_map's kernel_map
 * variant (ml_io_map_unmappable): its pmap_map applies the attribute to
 * managed pages too, and unlike ml_io_map_wcomb it does not draw on the
 * 8 MiB I/O submap, which the GMU's 16 MiB of code space alone exceeds.
 *
 * Aliasing rule (the same one Linux follows for non-coherent DMA): the
 * descriptor's own cacheable kernel mapping is zeroed, cleaned and
 * invalidated to PoC once, and then never written again; all CPU access
 * goes through the WC map. No dirty line can therefore be written back
 * over data the device produced.
 *
 * Physical address limit: SMMU output size on MMU-500 SM8250 is at least
 * 36 bits (read from ID2 at stage 3); RAM on apollo ends below 0x2_8000_0000,
 * so a 36-bit mask never excludes usable memory.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <sys/errno.h>
#include <vm/memory_types.h>

extern "C" {
#include "../../../bsd/dev/sweet_gpu.h"
void flush_dcache64(addr64_t addr, unsigned count, int phys);
vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t flags);
void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
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
		vm_offset_t va = ml_io_map_unmappable((vm_offset_t)pa, size, VM_WIMG_WCOMB);
		if (va == 0) {
			md->complete();
			md->release();
			bzero(out, sizeof(*out));
			return ENOMEM;
		}
		out->os_map = (void *)va;
		out->va = (void *)va;
	} else {
		out->va = cva;
	}
	return 0;
}

extern "C" void
sgpu_dma_free(struct sgpu_dma *dma)
{
	if (dma->os_map != NULL) {
		ml_io_unmap((vm_offset_t)dma->os_map, dma->size);
	}
	if (dma->os_md != NULL) {
		IOBufferMemoryDescriptor *md = (IOBufferMemoryDescriptor *)dma->os_md;
		md->complete();
		md->release();
	}
	bzero(dma, sizeof(*dma));
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
