/*
 * SweetAdreno - IOKit front end of the SWEET Adreno 650 driver.
 *
 * Kernel side of sweet_gpu_uapi.h. The hardware code is plain C
 * (bsd/dev/sweet_gpu_*.c); this file only does what needs IOKit:
 *
 *   - start a bring-up thread that runs sgpu_bringup() to the stage in
 *     boot-arg sweetgpu=N (default 0: nothing happens), after an optional
 *     delay sweetgpudelay=S seconds so a hang never blocks early boot;
 *   - publish the service (IOUserClientClass SweetAdrenoUserClient) only
 *     when the GPU reached the "user" stage, so Metal/turnip never see a
 *     half-initialised GPU;
 *   - buffers: IOBufferMemoryDescriptor pages, wired, mapped into the GPU
 *     address space (SMMU) and into the client task;
 *   - IOSurface import: the SweetSurface pixel memory mapped into the GPU,
 *     which is how CoreAnimation output will reach the GPU later;
 *   - deferred frees: a buffer is unmapped from the GPU only after every
 *     submission queued before the free has retired (ring fence).
 *
 * Diagnostics are mirrored into IORegistry properties of the service
 * ("sweetgpu-stage", "sweetgpu-error", "sweetgpu-message"), readable with
 * ioreg even when the user client is not published.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>
#include <sys/errno.h>

#include "SweetAccelerator.h"
#include "SweetSurface.h"

extern "C" {
#include "../../../bsd/dev/sweet_gpu.h"
}

#define SGPU_MAX_CLIENT_MEM     65536
#define SGPU_MAX_WIRED          (1536ull << 20)         /* all GPU buffers together */

/* ---------------------------------------------------------------- */

static struct sgpu sgpu_dev;
static IOLock    *sgpu_lock;            /* serialises every sgpu_* call */
static uint64_t   sgpu_wired;

struct sgpu_umem {
	struct sgpu_umem         *next;
	uint32_t                  id;
	uint32_t                  flags;
	uint64_t                  gpuaddr;
	uint64_t                  size;
	IOMemoryDescriptor       *md;           /* retained */
	IOMemoryMap              *userMap;      /* in the owning task, may be NULL */
	SweetSurface             *surface;      /* retained for imports */
	bool                      prepared;
};

/* Freed buffers wait here until the GPU is past them. */
struct sgpu_deferred {
	struct sgpu_deferred     *next;
	struct sgpu_umem         *mem;
	uint32_t                  fence;
};
static struct sgpu_deferred *sgpu_deferred_head;

/* Unmap from the GPU and drop the memory. Called with sgpu_lock held. */
static void
sgpu_umem_destroy(struct sgpu_umem *m)
{
	if (m->gpuaddr) {
		sgpu_smmu_unmap(&sgpu_dev, SGPU_DOM_GPU, m->gpuaddr, m->size);
		sgpu_va_free(&sgpu_dev, m->gpuaddr, m->size);
	}
	if (m->md) {
		if (m->prepared) {
			m->md->complete();
		}
		m->md->release();
	}
	if (m->surface) {
		m->surface->release();
	} else {
		sgpu_wired -= m->size;
	}
	IOFreeType(m, struct sgpu_umem);
}

static void
sgpu_reap_deferred(void)
{
	uint32_t retired = sgpu_cp_ring_fence_retired(&sgpu_dev);
	struct sgpu_deferred **pp = &sgpu_deferred_head;

	while (*pp) {
		struct sgpu_deferred *d = *pp;
		if (sgpu_ts_ge(retired, d->fence)) {
			*pp = d->next;
			sgpu_umem_destroy(d->mem);
			IOFreeType(d, struct sgpu_deferred);
		} else {
			pp = &d->next;
		}
	}
}

static void
sgpu_umem_release(struct sgpu_umem *m)
{
	if (m->userMap) {
		m->userMap->release();      /* the task's mapping goes now */
		m->userMap = NULL;
	}
	struct sgpu_deferred *d = IOMallocType(struct sgpu_deferred);
	d->mem = m;
	d->fence = sgpu_cp_ring_fence_submitted(&sgpu_dev);
	d->next = sgpu_deferred_head;
	sgpu_deferred_head = d;
	sgpu_reap_deferred();
}

/* Map every physical segment of md at gpuaddr. */
static int
sgpu_map_descriptor(IOMemoryDescriptor *md, uint64_t gpuaddr, uint64_t size, uint32_t prot)
{
	uint64_t off = 0;

	while (off < size) {
		IOByteCount seglen = 0;
		addr64_t pa = md->getPhysicalSegment(off, &seglen, kIOMemoryMapperNone);
		if (pa == 0 || seglen == 0) {
			return -EFAULT;
		}
		if (seglen > size - off) {
			seglen = (IOByteCount)(size - off);
		}
		int err = sgpu_smmu_map(&sgpu_dev, SGPU_DOM_GPU, gpuaddr + off, pa, seglen, prot);
		if (err) {
			if (off) {
				sgpu_smmu_unmap(&sgpu_dev, SGPU_DOM_GPU, gpuaddr, off);
			}
			return err;
		}
		off += seglen;
	}
	sgpu_smmu_tlb_sync(&sgpu_dev, SGPU_DOM_GPU);
	return 0;
}

static IOReturn
sgpu_ioreturn(int err)
{
	switch (-err) {
	case 0:         return kIOReturnSuccess;
	case ENOMEM:    return kIOReturnNoMemory;
	case EINVAL:    return kIOReturnBadArgument;
	case ENOENT:    return kIOReturnNotFound;
	case ETIMEDOUT: return kIOReturnTimeout;
	case EBUSY:     return kIOReturnBusy;
	case EAGAIN:    return kIOReturnNotReady;
	case ENODEV:    return kIOReturnNoDevice;
	case ENOTSUP:   return kIOReturnUnsupported;
	case EPERM:     return kIOReturnNotPrivileged;
	default:        return kIOReturnIOError;
	}
}

/* ---------------------------------------------------------------- */

/*
 * An IOAcceleratorES, because that is the class Metal and QuartzCore match
 * the GPU by; the user client, though, is IOService's (IOUserClientClass),
 * not the placeholder's refusal.
 */
class SweetAdreno : public IOAcceleratorES {
	OSDeclareDefaultStructors(SweetAdreno);

	uint32_t fStage;
	uint32_t fDelay;

	static void bringupThread(void *arg, wait_result_t wr);
	void publishStatus();

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
	    IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetAdreno, IOAcceleratorES);

IOReturn
SweetAdreno::newUserClient(task_t owningTask, void *securityID, UInt32 type,
    IOUserClient **handler)
{
	return IOService::newUserClient(owningTask, securityID, type, handler);
}

void
SweetAdreno::publishStatus()
{
	setProperty("sweetgpu-stage", (unsigned long long)sgpu_dev.stage_reached, 32);
	setProperty("sweetgpu-requested", (unsigned long long)sgpu_dev.stage_requested, 32);
	setProperty("sweetgpu-error", (unsigned long long)(uint32_t)sgpu_dev.last_error, 32);
	setProperty("sweetgpu-message", sgpu_dev.message[0] ? sgpu_dev.message : "ok");
}

void
SweetAdreno::bringupThread(void *arg, __unused wait_result_t wr)
{
	SweetAdreno *self = (SweetAdreno *)arg;

	if (self->fDelay) {
		IOLog("SWEET GPU: bring-up in %u s\n", self->fDelay);
		IOSleep(self->fDelay * 1000);
	}
	IOLockLock(sgpu_lock);
	int err = sgpu_bringup(&sgpu_dev, self->fStage);
	IOLockUnlock(sgpu_lock);
	self->publishStatus();
	if (!err && sgpu_dev.stage_reached >= SGPU_STAGE_USER) {
		self->setProperty(kIOUserClientClassKey, "SweetAdrenoUserClient");
		/*
		 * Metal plug-in discovery (docs/METAL.md): Metal matches every
		 * IOAcceleratorES, loads /System/Library/Extensions/
		 * <MetalPluginName>.bundle and instantiates <MetalPluginClassName>
		 * (a _MTLDevice subclass) with -initWithAcceleratorPort:. Off
		 * unless sweetmetal=1: as soon as a Metal device exists, QuartzCore
		 * leaves its working software renderer for Metal.
		 */
		uint32_t metal = 0;
		PE_parse_boot_argn("sweetmetal", &metal, sizeof(metal));
		if (metal == 1) {
			self->setProperty("MetalPluginName", "SweetMetal");
			self->setProperty("MetalPluginClassName", "SweetMetalDevice");
			IOLog("SWEET GPU: Metal plug-in named (sweetmetal=1)\n");
		}
		self->registerService();
		IOLog("SWEET GPU: SweetAdreno published\n");
	} else {
		IOLog("SWEET GPU: not published (stage %u of %u, error %d)\n",
		    sgpu_dev.stage_reached, self->fStage, err);
	}
	if (!err && sgpu_dev.stage_reached >= SGPU_STAGE_CP) {
		/* This thread stays as the DCVS tick: lowest level once idle. */
		for (;;) {
			IOSleep(50);
			IOLockLock(sgpu_lock);
			sgpu_dcvs_tick(&sgpu_dev);
			IOLockUnlock(sgpu_lock);
		}
	}
	self->release();
	thread_terminate(current_thread());
}

bool
SweetAdreno::start(IOService *provider)
{
	fStage = 0;
	fDelay = 10;
	PE_parse_boot_argn("sweetgpu", &fStage, sizeof(fStage));
	PE_parse_boot_argn("sweetgpudelay", &fDelay, sizeof(fDelay));
	uint32_t coherent = 0;
	PE_parse_boot_argn("sweetgpu_coherent", &coherent, sizeof(coherent));
	sgpu_dev.io_coherent = coherent == 1;
	if (fStage == 0) {
		return false;   /* default: no service, no register access */
	}
	if (!IOService::start(provider)) {
		return false;
	}
	sgpu_lock = IOLockAlloc();
	if (sgpu_lock == NULL) {
		return false;
	}
	publishStatus();
	retain();
	thread_t thread;
	if (kernel_thread_start(&SweetAdreno::bringupThread, this, &thread) != KERN_SUCCESS) {
		release();
		return false;
	}
	thread_deallocate(thread);
	return true;
}

/* ---------------------------------------------------------------- */

class SweetAdrenoUserClient : public IOUserClient {
	OSDeclareDefaultStructors(SweetAdrenoUserClient);

	task_t            fTask;
	struct sgpu_umem *fMem;
	uint64_t          fCtxMask;     /* contexts owned (bit = ctx id) */

	struct sgpu_umem *findMem(uint32_t id, struct sgpu_umem ***link);
	uint32_t allocId();
	bool ibInClientMemory(uint64_t gpuaddr, uint64_t bytes);
	IOReturn memAlloc(struct sgpu_mem_alloc *req);
	IOReturn importSurface(struct sgpu_import_surface *req);
	IOReturn importUser(struct sgpu_import_user *req);
	IOReturn submit(struct sgpu_submit *req);
	IOReturn waitTimestamp(const struct sgpu_wait_ts *req);
	IOReturn syncCache(const struct sgpu_sync_cache *req);

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetAdrenoUserClient, IOUserClient);

bool
SweetAdrenoUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	fMem = NULL;
	fCtxMask = 0;
	return true;
}

IOReturn
SweetAdrenoUserClient::clientClose(void)
{
	IOLockLock(sgpu_lock);
	/* Let this client's work drain (bounded), then drop everything it owns. */
	for (uint32_t c = 0; c < SGPU_MAX_CTX; c++) {
		if (!(fCtxMask & (1ull << c))) {
			continue;
		}
		uint32_t q = sgpu_cp_queued(&sgpu_dev, c);
		for (int i = 0; i < 1000 && !sgpu_ts_ge(sgpu_cp_retired(&sgpu_dev, c), q); i++) {
			IOLockUnlock(sgpu_lock);
			IOSleep(1);
			IOLockLock(sgpu_lock);
		}
		sgpu_cp_ctx_free(&sgpu_dev, c);
	}
	fCtxMask = 0;
	while (fMem) {
		struct sgpu_umem *m = fMem;
		fMem = m->next;
		sgpu_umem_release(m);
	}
	IOLockUnlock(sgpu_lock);
	terminate();
	return kIOReturnSuccess;
}

struct sgpu_umem *
SweetAdrenoUserClient::findMem(uint32_t id, struct sgpu_umem ***link)
{
	struct sgpu_umem **pp = &fMem;

	for (; *pp; pp = &(*pp)->next) {
		if ((*pp)->id == id) {
			if (link) {
				*link = pp;
			}
			return *pp;
		}
	}
	return NULL;
}

bool
SweetAdrenoUserClient::ibInClientMemory(uint64_t gpuaddr, uint64_t bytes)
{
	for (struct sgpu_umem *m = fMem; m; m = m->next) {
		if (gpuaddr >= m->gpuaddr && bytes <= m->size &&
		    gpuaddr - m->gpuaddr <= m->size - bytes) {
			return true;
		}
	}
	return false;
}

/*
 * Lowest free id of this client. turnip uses the id as the index of its
 * per-device sparse BO array (tu_device_lookup_bo), so ids must stay
 * small and get reused, like KGSL's.
 */
uint32_t
SweetAdrenoUserClient::allocId()
{
	for (uint32_t id = 1;; id++) {
		if (findMem(id, NULL) == NULL) {
			return id;
		}
	}
}

IOReturn
SweetAdrenoUserClient::memAlloc(struct sgpu_mem_alloc *req)
{
	uint64_t size = (req->size + PAGE_SIZE - 1) & ~(uint64_t)PAGE_MASK;
	uint64_t align = req->align_log2 ? (1ull << req->align_log2) : PAGE_SIZE;

	if (size == 0 || size > (1ull << 32) || req->align_log2 > 32) {
		return kIOReturnBadArgument;
	}
	if (sgpu_wired + size > SGPU_MAX_WIRED) {
		return kIOReturnNoMemory;
	}
	IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared, size, PAGE_SIZE);
	if (md == NULL) {
		return kIOReturnNoMemory;
	}
	bzero(md->getBytesNoCopy(), size);
	/* the client maps this write-combined: drop the dirty zero lines of
	 * the kernel's cacheable alias so they can never be written back over
	 * data the client or the GPU stored later */
	sgpu_cache_clean(md->getBytesNoCopy(), (size_t)size);
	if (md->prepare() != kIOReturnSuccess) {
		md->release();
		return kIOReturnNoMemory;
	}
	struct sgpu_umem *m = IOMallocType(struct sgpu_umem);
	m->md = md;
	m->prepared = true;
	m->size = size;
	m->flags = req->flags;
	sgpu_wired += size;

	uint32_t prot = SGPU_PROT_READ;
	if (!(req->flags & SGPU_MEM_GPU_READONLY)) {
		prot |= SGPU_PROT_WRITE;
	}
	if (req->flags & SGPU_MEM_GPU_EXEC) {
		prot |= SGPU_PROT_EXEC;
	}
	if (req->flags & SGPU_MEM_CACHED) {
		prot |= SGPU_PROT_CACHED;       /* inner-shareable WB: snooped if IO-coherent */
	}
	int err = sgpu_va_alloc(&sgpu_dev, false, size, align, &m->gpuaddr);
	if (!err) {
		err = sgpu_map_descriptor(md, m->gpuaddr, size, prot);
		if (err) {
			sgpu_va_free(&sgpu_dev, m->gpuaddr, size);
		}
	}
	if (err) {
		m->gpuaddr = 0;
		sgpu_umem_destroy(m);
		return sgpu_ioreturn(err);
	}
	if (!(req->flags & SGPU_MEM_NO_CPU_MAP)) {
		IOOptionBits cache = (req->flags & SGPU_MEM_CACHED) ?
		    kIOMapDefaultCache : kIOMapWriteCombineCache;
		m->userMap = md->createMappingInTask(fTask, 0, kIOMapAnywhere | cache);
		if (m->userMap == NULL) {
			sgpu_umem_destroy(m);
			return kIOReturnNoMemory;
		}
	}
	m->id = allocId();
	m->next = fMem;
	fMem = m;

	req->id = m->id;
	req->gpuaddr = m->gpuaddr;
	req->cpu_addr = m->userMap ? m->userMap->getAddress() : 0;
	req->alloc_size = size;
	return kIOReturnSuccess;
}

IOReturn
SweetAdrenoUserClient::importUser(struct sgpu_import_user *req)
{
	if (req->size == 0 || req->size > (1ull << 32) || (req->addr & PAGE_MASK) ||
	    (req->size & PAGE_MASK)) {
		return kIOReturnBadArgument;
	}
	if (sgpu_wired + req->size > SGPU_MAX_WIRED) {
		return kIOReturnNoMemory;
	}
	IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(req->addr, req->size,
	    kIODirectionInOut, fTask);
	if (md == NULL) {
		return kIOReturnNoMemory;
	}
	struct sgpu_umem *m = IOMallocType(struct sgpu_umem);
	m->md = md;
	m->size = req->size;
	m->flags = req->flags;
	sgpu_wired += req->size;        /* given back by sgpu_umem_destroy */
	m->prepared = md->prepare() == kIOReturnSuccess;     /* wires the pages */
	if (!m->prepared) {
		sgpu_umem_destroy(m);
		return kIOReturnVMError;
	}
	/* the GPU does not snoop: no dirty line may be written back over what
	 * it renders, and no stale one may hide it */
	md->performOperation(kIOMemoryIncoherentIOFlush, 0, req->size);
	uint32_t prot = SGPU_PROT_READ;
	if (!(req->flags & SGPU_MEM_GPU_READONLY)) {
		prot |= SGPU_PROT_WRITE;
	}
	int err = sgpu_va_alloc(&sgpu_dev, false, req->size, PAGE_SIZE, &m->gpuaddr);
	if (!err) {
		err = sgpu_map_descriptor(m->md, m->gpuaddr, req->size, prot);
		if (err) {
			sgpu_va_free(&sgpu_dev, m->gpuaddr, req->size);
		}
	}
	if (err) {
		m->gpuaddr = 0;
		sgpu_umem_destroy(m);
		return sgpu_ioreturn(err);
	}
	m->id = allocId();
	m->next = fMem;
	fMem = m;
	req->id = m->id;
	req->gpuaddr = m->gpuaddr;
	return kIOReturnSuccess;
}

IOReturn
SweetAdrenoUserClient::importSurface(struct sgpu_import_surface *req)
{
	SweetSurface *s = sweet_surface_copy(req->surface_id);
	if (s == NULL || s->memory == NULL) {
		OSSafeReleaseNULL(s);
		return kIOReturnNotFound;
	}
	uint64_t size = (s->memory->getLength() + PAGE_SIZE - 1) & ~(uint64_t)PAGE_MASK;
	struct sgpu_umem *m = IOMallocType(struct sgpu_umem);
	m->surface = s;
	m->size = size;
	m->flags = req->flags;
	s->memory->retain();
	m->md = s->memory;
	m->prepared = m->md->prepare() == kIOReturnSuccess;
	if (!m->prepared) {
		sgpu_umem_destroy(m);
		return kIOReturnNoMemory;
	}
	m->md->performOperation(kIOMemoryIncoherentIOFlush, 0, m->md->getLength());
	uint32_t prot = SGPU_PROT_READ;
	if (!(req->flags & SGPU_MEM_GPU_READONLY)) {
		prot |= SGPU_PROT_WRITE;
	}
	int err = sgpu_va_alloc(&sgpu_dev, false, size, PAGE_SIZE, &m->gpuaddr);
	if (!err) {
		err = sgpu_map_descriptor(m->md, m->gpuaddr, size, prot);
		if (err) {
			sgpu_va_free(&sgpu_dev, m->gpuaddr, size);
		}
	}
	if (err) {
		m->gpuaddr = 0;
		sgpu_umem_destroy(m);
		return sgpu_ioreturn(err);
	}
	m->id = allocId();
	m->next = fMem;
	fMem = m;
	req->id = m->id;
	req->gpuaddr = m->gpuaddr;
	req->size = size;
	return kIOReturnSuccess;
}

IOReturn
SweetAdrenoUserClient::submit(struct sgpu_submit *req)
{
	if (req->ctx_id >= SGPU_MAX_CTX || !(fCtxMask & (1ull << req->ctx_id)) ||
	    req->num_ibs > SGPU_MAX_IBS) {
		return kIOReturnBadArgument;
	}
	for (uint32_t i = 0; i < req->num_ibs; i++) {
		uint64_t bytes = (uint64_t)req->ibs[i].size_dwords * 4;
		if (bytes == 0 || bytes > (1u << 22) || (req->ibs[i].gpuaddr & 3) ||
		    !ibInClientMemory(req->ibs[i].gpuaddr, bytes)) {
			return kIOReturnBadArgument;
		}
	}
	int err = sgpu_cp_submit(&sgpu_dev, req->ctx_id, req->ibs, req->num_ibs,
	    req->flags, &req->timestamp);
	sgpu_reap_deferred();
	return sgpu_ioreturn(err);
}

IOReturn
SweetAdrenoUserClient::waitTimestamp(const struct sgpu_wait_ts *req)
{
	if (req->ctx_id >= SGPU_MAX_CTX || !(fCtxMask & (1ull << req->ctx_id))) {
		return kIOReturnBadArgument;
	}
	uint64_t start = sgpu_now_us();
	uint32_t sleep_us = 50;

	for (;;) {
		if (sgpu_ts_ge(sgpu_cp_retired(&sgpu_dev, req->ctx_id), req->timestamp)) {
			sgpu_reap_deferred();
			return kIOReturnSuccess;
		}
		if (sgpu_cp_hang_check(&sgpu_dev)) {
			return kIOReturnIOError;
		}
		if (sgpu_now_us() - start >= (uint64_t)req->timeout_ms * 1000) {
			return kIOReturnTimeout;
		}
		/* No GPU interrupt yet (GIC SPI 332 not wired): poll with backoff. */
		IOLockUnlock(sgpu_lock);
		IODelay(sleep_us < 1000 ? sleep_us : 0);
		if (sleep_us >= 1000) {
			IOSleep(1);
		}
		IOLockLock(sgpu_lock);
		if (sleep_us < 1000) {
			sleep_us *= 2;
		}
	}
}

IOReturn
SweetAdrenoUserClient::syncCache(const struct sgpu_sync_cache *req)
{
	struct sgpu_umem *m = findMem(req->id, NULL);
	if (m == NULL || req->offset > m->size) {
		return kIOReturnBadArgument;
	}
	uint64_t len = req->length ? req->length : m->size - req->offset;
	if (len > m->size - req->offset) {
		return kIOReturnBadArgument;
	}
	/*
	 * Needed for SGPU_MEM_CACHED buffers unless the GPU path is proven
	 * IO-coherent (boot-arg sweetgpu_coherent=1 -> SGPU_INFO_IO_COHERENT);
	 * the upstream and stock device trees disagree on that, so clients
	 * must assume it is not.
	 */
	IOMemoryMap *km = m->md->map(kIOMapDefaultCache);
	if (km == NULL) {
		return kIOReturnNoMemory;
	}
	const void *va = (const void *)(km->getVirtualAddress() + req->offset);
	if (req->op & SGPU_CACHE_CLEAN) {
		sgpu_cache_clean(va, (size_t)len);
	}
	if (req->op & SGPU_CACHE_INVALIDATE) {
		sgpu_cache_inval(va, (size_t)len);
	}
	km->release();
	return kIOReturnSuccess;
}

static IOReturn
struct_in(IOExternalMethodArguments *a, void *buf, uint32_t len)
{
	if (a->structureInputDescriptor != NULL) {
		if (a->structureInputDescriptor->getLength() != len) {
			return kIOReturnBadArgument;
		}
		a->structureInputDescriptor->readBytes(0, buf, len);
		return kIOReturnSuccess;
	}
	if (a->structureInput == NULL || a->structureInputSize != len) {
		return kIOReturnBadArgument;
	}
	memcpy(buf, a->structureInput, len);
	return kIOReturnSuccess;
}

static IOReturn
struct_out(IOExternalMethodArguments *a, const void *buf, uint32_t len)
{
	if (a->structureOutputDescriptor != NULL) {
		if (a->structureOutputDescriptor->getLength() < len) {
			return kIOReturnNoSpace;
		}
		a->structureOutputDescriptor->writeBytes(0, buf, len);
		a->structureOutputDescriptorSize = len;
		return kIOReturnSuccess;
	}
	if (a->structureOutput == NULL || a->structureOutputSize < len) {
		return kIOReturnNoSpace;
	}
	memcpy(a->structureOutput, buf, len);
	a->structureOutputSize = len;
	return kIOReturnSuccess;
}

IOReturn
SweetAdrenoUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	union {
		struct sgpu_info           info;
		struct sgpu_ctx_create     ctx;
		struct sgpu_ctx_destroy    ctxd;
		struct sgpu_mem_alloc      alloc;
		struct sgpu_mem_free       free;
		struct sgpu_import_surface import;
		struct sgpu_import_user    user;
		struct sgpu_wait_ts        wait;
		struct sgpu_read_ts        rts;
		struct sgpu_sync_cache     sync;
		struct sgpu_gpu_time       time;
		struct sgpu_status         status;
	} u;
	struct sgpu_submit *sub = NULL;
	IOReturn ret;

	bzero(&u, sizeof(u));
	IOLockLock(sgpu_lock);
	switch (selector) {
	case SGPU_SEL_GET_INFO:
		sgpu_fill_info(&sgpu_dev, &u.info);
		ret = struct_out(args, &u.info, sizeof(u.info));
		break;
	case SGPU_SEL_GET_STATUS:
		sgpu_cp_status(&sgpu_dev, &u.status);
		ret = struct_out(args, &u.status, sizeof(u.status));
		break;
	case SGPU_SEL_CTX_CREATE:
		ret = struct_in(args, &u.ctx, sizeof(u.ctx));
		if (ret == kIOReturnSuccess) {
			ret = sgpu_ioreturn(sgpu_cp_ctx_alloc(&sgpu_dev, &u.ctx.ctx_id));
		}
		if (ret == kIOReturnSuccess) {
			fCtxMask |= 1ull << u.ctx.ctx_id;
			ret = struct_out(args, &u.ctx, sizeof(u.ctx));
		}
		break;
	case SGPU_SEL_CTX_DESTROY:
		ret = struct_in(args, &u.ctxd, sizeof(u.ctxd));
		if (ret == kIOReturnSuccess) {
			if (u.ctxd.ctx_id >= SGPU_MAX_CTX || !(fCtxMask & (1ull << u.ctxd.ctx_id))) {
				ret = kIOReturnBadArgument;
			} else {
				sgpu_cp_ctx_free(&sgpu_dev, u.ctxd.ctx_id);
				fCtxMask &= ~(1ull << u.ctxd.ctx_id);
			}
		}
		break;
	case SGPU_SEL_MEM_ALLOC:
		ret = struct_in(args, &u.alloc, sizeof(u.alloc));
		if (ret == kIOReturnSuccess) {
			ret = memAlloc(&u.alloc);
		}
		if (ret == kIOReturnSuccess) {
			ret = struct_out(args, &u.alloc, sizeof(u.alloc));
		}
		break;
	case SGPU_SEL_MEM_FREE: {
		struct sgpu_umem **link = NULL;
		ret = struct_in(args, &u.free, sizeof(u.free));
		if (ret == kIOReturnSuccess) {
			struct sgpu_umem *m = findMem(u.free.id, &link);
			if (m == NULL) {
				ret = kIOReturnNotFound;
			} else {
				*link = m->next;
				sgpu_umem_release(m);
			}
		}
		break;
	}
	case SGPU_SEL_IMPORT_SURFACE:
		ret = struct_in(args, &u.import, sizeof(u.import));
		if (ret == kIOReturnSuccess) {
			ret = importSurface(&u.import);
		}
		if (ret == kIOReturnSuccess) {
			ret = struct_out(args, &u.import, sizeof(u.import));
		}
		break;
	case SGPU_SEL_IMPORT_USER:
		ret = struct_in(args, &u.user, sizeof(u.user));
		if (ret == kIOReturnSuccess) {
			ret = importUser(&u.user);
		}
		if (ret == kIOReturnSuccess) {
			ret = struct_out(args, &u.user, sizeof(u.user));
		}
		break;
	case SGPU_SEL_SYNC_CACHE:
		ret = struct_in(args, &u.sync, sizeof(u.sync));
		if (ret == kIOReturnSuccess) {
			ret = syncCache(&u.sync);
		}
		break;
	case SGPU_SEL_SUBMIT:
		sub = IOMallocType(struct sgpu_submit);
		ret = struct_in(args, sub, sizeof(*sub));
		if (ret == kIOReturnSuccess) {
			ret = submit(sub);
		}
		if (ret == kIOReturnSuccess) {
			ret = struct_out(args, sub, sizeof(*sub));
		}
		IOFreeType(sub, struct sgpu_submit);
		break;
	case SGPU_SEL_WAIT_TS:
		ret = struct_in(args, &u.wait, sizeof(u.wait));
		if (ret == kIOReturnSuccess) {
			ret = waitTimestamp(&u.wait);
		}
		break;
	case SGPU_SEL_READ_TS:
		ret = struct_in(args, &u.rts, sizeof(u.rts));
		if (ret == kIOReturnSuccess) {
			if (u.rts.ctx_id >= SGPU_MAX_CTX || !(fCtxMask & (1ull << u.rts.ctx_id))) {
				ret = kIOReturnBadArgument;
			} else {
				u.rts.retired = sgpu_cp_retired(&sgpu_dev, u.rts.ctx_id);
				u.rts.queued = sgpu_cp_queued(&sgpu_dev, u.rts.ctx_id);
				ret = struct_out(args, &u.rts, sizeof(u.rts));
			}
		}
		break;
	case SGPU_SEL_GPU_TIME:
		u.time.gpu_ticks = sgpu_cp_always_on(&sgpu_dev);
		u.time.cpu_ticks = mach_absolute_time();
		ret = struct_out(args, &u.time, sizeof(u.time));
		break;
	default:
		ret = kIOReturnUnsupported;
		break;
	}
	IOLockUnlock(sgpu_lock);
	return ret;
}
#endif /* SWEET */
