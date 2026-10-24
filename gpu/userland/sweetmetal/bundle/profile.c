/*
 * A sampling profiler of the thread that commits command buffers (for
 * CoreAnimation, its render thread), in the process itself: with
 * SWEETMETAL_PROFILE set, a thread stops it every millisecond, walks its
 * frame pointers and counts each function, by the nearest symbol dladdr
 * finds. Every 10 s it prints the functions most often on the CPU (self)
 * and on the stack (total) to stderr, and starts over.
 */
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "profile.h"

#define SLOTS   4096
#define DEPTH   48
#define SHOW    45

struct slot {
	uintptr_t addr;         /* symbol start, 0 = free */
	uint32_t self, total;
	uint32_t seen;          /* sample number that last counted it in total */
};

static _Atomic thread_act_t target = MACH_PORT_NULL;
static struct slot slots[SLOTS];
static uint32_t samples, lost, idle;

static struct slot *
slot_for(uintptr_t pc)
{
	Dl_info info;
	uintptr_t sym = pc;
	if (dladdr((void *)pc, &info) != 0 && info.dli_saddr != NULL) {
		sym = (uintptr_t)info.dli_saddr;
	}
	for (uint32_t i = (uint32_t)(sym >> 2) % SLOTS, n = 0; n < SLOTS; i = (i + 1) % SLOTS, n++) {
		if (slots[i].addr == sym) {
			return &slots[i];
		}
		if (slots[i].addr == 0) {
			slots[i].addr = sym;
			return &slots[i];
		}
	}
	return NULL;
}

static void
sample(thread_act_t t)
{
	arm_thread_state64_t st;
	mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
	uintptr_t pcs[DEPTH];
	uint32_t depth = 0;

	if (thread_suspend(t) != KERN_SUCCESS) {
		lost++;
		return;
	}
	if (thread_get_state(t, ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS) {
		pcs[depth++] = (uintptr_t)arm_thread_state64_get_pc(st);
		pcs[depth++] = (uintptr_t)arm_thread_state64_get_lr(st);
		uintptr_t fp = (uintptr_t)arm_thread_state64_get_fp(st);
		/* frame records: [fp] = caller's fp, [fp + 8] = return address */
		while (depth < DEPTH && fp != 0 && (fp & 7) == 0) {
			vm_size_t got = 0;
			uintptr_t rec[2];
			if (vm_read_overwrite(mach_task_self(), fp, sizeof(rec), (vm_address_t)rec,
			    &got) != KERN_SUCCESS || got != sizeof(rec) || rec[0] <= fp) {
				break;
			}
			pcs[depth++] = rec[1] & 0x0000000fffffffffull;
			fp = rec[0];
		}
	}
	thread_resume(t);
	if (depth == 0) {
		lost++;
		return;
	}
	/* a thread waiting (for a message or a condition) does no work: count it apart */
	Dl_info info;
	if (dladdr((void *)pcs[0], &info) != 0 && info.dli_sname != NULL &&
	    (strcmp(info.dli_sname, "mach_msg2_trap") == 0 ||
	    strcmp(info.dli_sname, "__psynch_cvwait") == 0 ||
	    strcmp(info.dli_sname, "__psynch_mutexwait") == 0 ||
	    strcmp(info.dli_sname, "__semwait_signal") == 0)) {
		idle++;
		return;
	}
	samples++;
	for (uint32_t i = 0; i < depth; i++) {
		struct slot *s = slot_for(pcs[i]);
		if (s == NULL) {
			continue;
		}
		if (i == 0) {
			s->self++;
		}
		if (s->seen != samples) {
			s->seen = samples;
			s->total++;
		}
	}
}

static int
by_self(const void *a, const void *b)
{
	return (int)((const struct slot *)b)->self - (int)((const struct slot *)a)->self;
}

static int
by_total(const void *a, const void *b)
{
	return (int)((const struct slot *)b)->total - (int)((const struct slot *)a)->total;
}

static void
print_top(const char *what, int (*cmp)(const void *, const void *))
{
	static struct slot sorted[SLOTS];
	memcpy(sorted, slots, sizeof(sorted));
	qsort(sorted, SLOTS, sizeof(sorted[0]), cmp);
	for (int i = 0; i < SHOW && sorted[i].addr != 0; i++) {
		uint32_t v = cmp == by_self ? sorted[i].self : sorted[i].total;
		if (v == 0) {
			break;
		}
		Dl_info info;
		const char *name = "?", *image = "?";
		if (dladdr((void *)sorted[i].addr, &info) != 0) {
			name = info.dli_sname ? info.dli_sname : "?";
			if (info.dli_fname) {
				image = strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 :
				    info.dli_fname;
			}
		}
		/* symbols the shared cache strips: the address, symbolized offline */
		if (strcmp(name, "<redacted>") == 0 || strcmp(name, "?") == 0) {
			fprintf(stderr, "smprof %s %5.1f%% %s`%#lx\n", what, 100.0 * v / samples, image,
			    (unsigned long)sorted[i].addr);
		} else {
			fprintf(stderr, "smprof %s %5.1f%% %s`%s\n", what, 100.0 * v / samples, image, name);
		}
	}
}

static void *
run(void *arg)
{
	(void)arg;
	pthread_setname_np("smprof");
	for (uint32_t tick = 1;; tick++) {
		usleep(1000);
		thread_act_t t = atomic_load(&target);
		if (t != MACH_PORT_NULL) {
			sample(t);
		}
		if (tick % 10000 == 0 && samples != 0) {
			fprintf(stderr, "smprof: %u busy samples (%u waiting, %u lost) of the committing "
			    "thread\n", samples, idle, lost);
			print_top("self ", by_self);
			print_top("total", by_total);
			memset(slots, 0, sizeof(slots));
			samples = lost = idle = 0;
		}
	}
	return NULL;
}

void
smprof_start(void)
{
	static int started;
	if (started || getenv("SWEETMETAL_PROFILE") == NULL) {
		return;
	}
	started = 1;
	pthread_t th;
	pthread_create(&th, NULL, run, NULL);
}

void
smprof_note_thread(void)
{
	if (atomic_load(&target) == MACH_PORT_NULL) {
		atomic_store(&target, mach_thread_self());
	}
}
