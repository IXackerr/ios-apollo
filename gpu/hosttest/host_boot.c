/*
 * host_boot.c - entry point used when the host-test kernel + model is
 * built as a shared library (libsgpu_host.so) underneath a real Vulkan
 * driver: model reset + full bring-up, so turnip's tu_knl_sweet finds a
 * GPU at stage 8 through libsweetgpu -> sgpu_host_dispatch.
 */
#include <string.h>
#include "model.h"
#include "../kernel/sweet_gpu.h"

extern struct sgpu host_g;
extern int host_verbose;
void host_dispatch_reset(void);

__attribute__((visibility("default"))) int
sgpu_host_boot(int verbose, int permissive)
{
	struct model_opts o = { .zap_present = true, .speed_bin = 1,
		.permissive_pm4 = permissive != 0 };
	host_verbose = verbose;
	memset(&host_g, 0, sizeof(host_g));
	model_reset(&o);
	host_dispatch_reset();
	host_clear_bootargs();
	return sgpu_bringup(&host_g, SGPU_STAGE_USER);
}

__attribute__((visibility("default"))) int
sgpu_host_model_errors(void)
{
	return model_errors;
}

__attribute__((visibility("default"))) unsigned
sgpu_host_cp_stats(unsigned *skipped)
{
	*skipped = model_cp_skipped;
	return model_cp_packets;
}

/*
 * LD_PRELOAD=libsgpu_host.so SGPU_HOST_AUTOBOOT=1 <any vulkan program>:
 * bring the modelled GPU up before main(), so the Vulkan loader's turnip
 * (built with -Dfreedreno-kmds=sweet) finds a device. SGPU_HOST_VERBOSE=1
 * prints the kernel driver's log.
 */
#include <stdio.h>
#include <stdlib.h>
__attribute__((constructor)) static void
sgpu_host_autoboot(void)
{
	const char *a = getenv("SGPU_HOST_AUTOBOOT");
	if (a == NULL || a[0] != '1') {
		return;
	}
	const char *v = getenv("SGPU_HOST_VERBOSE");
	int err = sgpu_host_boot(v != NULL && v[0] == '1', 1);
	fprintf(stderr, "sgpu-host: modelled Adreno 650 bring-up %s (%d)\n", err ? "FAILED" : "ok", err);
}

__attribute__((destructor)) static void
sgpu_host_report(void)
{
	const char *a = getenv("SGPU_HOST_AUTOBOOT");
	if (a != NULL && a[0] == '1') {
		unsigned skipped, pk = sgpu_host_cp_stats(&skipped);
		fprintf(stderr, "sgpu-host: CP executed %u packets (%u skipped), model errors %d\n",
		    pk, skipped, sgpu_host_model_errors());
	}
}
