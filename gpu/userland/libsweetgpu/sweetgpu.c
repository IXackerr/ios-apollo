/*
 * libsweetgpu - see sweetgpu.h.
 *
 * On Darwin (the phone): IOKit user client calls. The iOS SDK ships
 * IOKit.framework without headers, so the handful of functions used are
 * declared here (same prototypes as macOS IOKit/IOKitLib.h).
 *
 * Elsewhere (Linux host tests): calls go to sgpu_host_dispatch(), which the
 * host test provides by linking the kernel C code in-process.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "sweetgpu.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>

typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;

extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type,
    io_connect_t *connect);
extern kern_return_t IOServiceClose(io_connect_t connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t selector,
    const void *inputStruct, size_t inputStructCnt, void *outputStruct, size_t *outputStructCnt);

struct sgpu_dev {
	io_connect_t conn;
};

static int
kr_to_errno(kern_return_t kr)
{
	switch ((uint32_t)kr) {
	case 0:          return 0;
	case 0xe00002bd: return -ENOMEM;     /* kIOReturnNoMemory */
	case 0xe00002c2: return -EINVAL;     /* kIOReturnBadArgument */
	case 0xe00002c7: return -ENOTSUP;    /* kIOReturnUnsupported */
	case 0xe00002c0: return -ENODEV;     /* kIOReturnNoDevice */
	case 0xe00002d5: return -EBUSY;      /* kIOReturnBusy */
	case 0xe00002d6: return -ETIMEDOUT;  /* kIOReturnTimeout */
	case 0xe00002d8: return -EAGAIN;     /* kIOReturnNotReady */
	case 0xe00002f0: return -ENOENT;     /* kIOReturnNotFound */
	case 0xe00002c1: return -EPERM;      /* kIOReturnNotPrivileged */
	case 0xe00002e2: return -EACCES;     /* kIOReturnNotPermitted */
	default:         return -EIO;
	}
}

static int
call(sgpu_dev *dev, uint32_t sel, const void *in, size_t inlen, void *out, size_t outlen)
{
	size_t got = outlen;
	kern_return_t kr = IOConnectCallStructMethod(dev->conn, sel, in, inlen,
	    out, out ? &got : NULL);
	if (kr == 0 && out && got != outlen) {
		return -EPROTO;
	}
	return kr_to_errno(kr);
}

int
sgpu_open(sgpu_dev **out)
{
	io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL,
	    IOServiceMatching(SGPU_SERVICE_CLASS));
	if (svc == MACH_PORT_NULL) {
		return -ENODEV;
	}
	sgpu_dev *dev = calloc(1, sizeof(*dev));
	if (!dev) {
		IOObjectRelease(svc);
		return -ENOMEM;
	}
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &dev->conn);
	IOObjectRelease(svc);
	if (kr != 0) {
		free(dev);
		return kr_to_errno(kr);
	}
	struct sgpu_info info;
	int err = call(dev, SGPU_SEL_GET_INFO, NULL, 0, &info, sizeof(info));
	if (!err && info.uapi_version != SGPU_UAPI_VERSION) {
		err = -EPROTO;
	}
	if (err) {
		IOServiceClose(dev->conn);
		free(dev);
		return err;
	}
	*out = dev;
	return 0;
}

void
sgpu_close(sgpu_dev *dev)
{
	if (dev) {
		IOServiceClose(dev->conn);
		free(dev);
	}
}

#else /* host test: in-process kernel */

/* Weak: a build without the host test (e.g. a Linux compile check of the
 * turnip backend) links and simply finds no device. */
extern int sgpu_host_dispatch(uint32_t sel, const void *in, size_t inlen,
    void *out, size_t *outlen) __attribute__((weak));

struct sgpu_dev {
	int unused;
};

static int
call(sgpu_dev *dev, uint32_t sel, const void *in, size_t inlen, void *out, size_t outlen)
{
	size_t got = outlen;
	(void)dev;
	if (sgpu_host_dispatch == NULL) {
		return -ENODEV;
	}
	int err = sgpu_host_dispatch(sel, in, inlen, out, &got);
	if (!err && out && got != outlen) {
		return -EPROTO;
	}
	return err;
}

int
sgpu_open(sgpu_dev **out)
{
	if (sgpu_host_dispatch == NULL) {
		return -ENODEV;
	}
	*out = calloc(1, sizeof(**out));
	return *out ? 0 : -ENOMEM;
}

void
sgpu_close(sgpu_dev *dev)
{
	free(dev);
}
#endif

int
sgpu_get_info(sgpu_dev *dev, struct sgpu_info *info)
{
	return call(dev, SGPU_SEL_GET_INFO, NULL, 0, info, sizeof(*info));
}

int
sgpu_get_status(sgpu_dev *dev, struct sgpu_status *st)
{
	return call(dev, SGPU_SEL_GET_STATUS, NULL, 0, st, sizeof(*st));
}

int
sgpu_ctx_create(sgpu_dev *dev, uint32_t flags, uint32_t priority, uint32_t *ctx_id)
{
	struct sgpu_ctx_create c = { .flags = flags, .priority = priority };
	int err = call(dev, SGPU_SEL_CTX_CREATE, &c, sizeof(c), &c, sizeof(c));
	if (!err) {
		*ctx_id = c.ctx_id;
	}
	return err;
}

int
sgpu_ctx_destroy(sgpu_dev *dev, uint32_t ctx_id)
{
	struct sgpu_ctx_destroy c = { .ctx_id = ctx_id };
	return call(dev, SGPU_SEL_CTX_DESTROY, &c, sizeof(c), NULL, 0);
}

int
sgpu_mem_alloc(sgpu_dev *dev, struct sgpu_mem_alloc *req)
{
	return call(dev, SGPU_SEL_MEM_ALLOC, req, sizeof(*req), req, sizeof(*req));
}

int
sgpu_mem_free(sgpu_dev *dev, uint32_t id)
{
	struct sgpu_mem_free f = { .id = id };
	return call(dev, SGPU_SEL_MEM_FREE, &f, sizeof(f), NULL, 0);
}

int
sgpu_mem_sync(sgpu_dev *dev, uint32_t id, uint32_t op, uint64_t offset, uint64_t length)
{
	struct sgpu_sync_cache s = { .id = id, .op = op, .offset = offset, .length = length };
	return call(dev, SGPU_SEL_SYNC_CACHE, &s, sizeof(s), NULL, 0);
}

int
sgpu_import_surface(sgpu_dev *dev, struct sgpu_import_surface *req)
{
	return call(dev, SGPU_SEL_IMPORT_SURFACE, req, sizeof(*req), req, sizeof(*req));
}

int
sgpu_import_user(sgpu_dev *dev, struct sgpu_import_user *req)
{
	return call(dev, SGPU_SEL_IMPORT_USER, req, sizeof(*req), req, sizeof(*req));
}

int
sgpu_surface_busy(sgpu_dev *dev, uint32_t surface_id, int32_t delta)
{
	struct sgpu_surface_busy b = { .surface_id = surface_id, .delta = delta };
	return call(dev, SGPU_SEL_SURFACE_BUSY, &b, sizeof(b), NULL, 0);
}

int
sgpu_submit(sgpu_dev *dev, uint32_t ctx_id, const struct sgpu_ib *ibs, uint32_t num_ibs,
    uint32_t flags, uint32_t *timestamp)
{
	struct sgpu_submit s;

	if (num_ibs > SGPU_MAX_IBS) {
		return -E2BIG;
	}
	memset(&s, 0, sizeof(s));
	s.ctx_id = ctx_id;
	s.num_ibs = num_ibs;
	s.flags = flags;
	if (num_ibs) {
		memcpy(s.ibs, ibs, num_ibs * sizeof(*ibs));
	}
	int err = call(dev, SGPU_SEL_SUBMIT, &s, sizeof(s), &s, sizeof(s));
	if (!err && timestamp) {
		*timestamp = s.timestamp;
	}
	return err;
}

int
sgpu_wait_ts(sgpu_dev *dev, uint32_t ctx_id, uint32_t ts, uint32_t timeout_ms)
{
	struct sgpu_wait_ts w = { .ctx_id = ctx_id, .timestamp = ts, .timeout_ms = timeout_ms };
	return call(dev, SGPU_SEL_WAIT_TS, &w, sizeof(w), NULL, 0);
}

int
sgpu_read_ts(sgpu_dev *dev, uint32_t ctx_id, uint32_t *retired, uint32_t *queued)
{
	struct sgpu_read_ts r = { .ctx_id = ctx_id };
	int err = call(dev, SGPU_SEL_READ_TS, &r, sizeof(r), &r, sizeof(r));
	if (!err) {
		if (retired) {
			*retired = r.retired;
		}
		if (queued) {
			*queued = r.queued;
		}
	}
	return err;
}

int
sgpu_gpu_time(sgpu_dev *dev, uint64_t *gpu_ticks, uint64_t *cpu_ticks)
{
	struct sgpu_gpu_time t;
	int err = call(dev, SGPU_SEL_GPU_TIME, NULL, 0, &t, sizeof(t));
	if (!err) {
		if (gpu_ticks) {
			*gpu_ticks = t.gpu_ticks;
		}
		if (cpu_ticks) {
			*cpu_ticks = t.cpu_ticks;
		}
	}
	return err;
}
