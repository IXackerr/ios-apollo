# Host test of the SWEET Adreno driver: real driver sources + SoC model.
CC      ?= cc
CFLAGS  ?= -O1 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Werror \
           -DSGPU_HOST -DSGPU_GPUTEST_NO_MAIN \
           -I../kernel -I../userland/libsweetgpu -fsanitize=address,undefined
DRIVER  := $(filter-out ../kernel/sweet_gpu_os_xnu.c,$(wildcard ../kernel/sweet_gpu_*.c))
SRCS    := $(DRIVER) model.c os_host.c dispatch.c main.c \
           ../userland/libsweetgpu/sweetgpu.c ../userland/gputest/gputest.c

sgpu_hosttest: $(SRCS) $(wildcard ../kernel/*.h) model.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

check: sgpu_hosttest
	./sgpu_hosttest

clean:
	rm -f sgpu_hosttest
.PHONY: check clean

# Vulkan end-to-end: real Mesa turnip (built with -Dfreedreno-kmds=sweet,
# see mesa/README.md) on top of the driver + model as a shared library.
MESA_SRC   ?= $(HOME)/mesa
MESA_BUILD ?= $(HOME)/mesa-build
HOSTLIB_SRCS := $(DRIVER) model.c os_host.c dispatch.c host_boot.c

libsgpu_host.so: $(HOSTLIB_SRCS) $(wildcard ../kernel/*.h) model.h
	$(CC) -std=gnu11 -O1 -g -fPIC -shared -DSGPU_HOST -I../kernel -o $@ $(HOSTLIB_SRCS)

vktest: vktest.c libsgpu_host.so
	$(CC) -std=gnu11 -O1 -g -I$(MESA_SRC)/include -o $@ vktest.c \
	    -Wl,--no-as-needed -L. -lsgpu_host -Wl,-rpath,'$$ORIGIN' -ldl

vkcheck: vktest
	./vktest $(MESA_BUILD)/src/freedreno/vulkan/libvulkan_freedreno.so
.PHONY: vkcheck
