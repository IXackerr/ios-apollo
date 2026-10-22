# gpu/ — Adreno 650 and Metal → Vulkan for iOS on apollo

```
kernel/        SweetAdreno: Adreno 650 driver for XNU (→ xnu/patches/0094)
  sweet_gpu_uapi.h   kernel/userspace ABI (KGSL-like)
  sweet_gpu.h        internal interface + sgpu_os_* OS layer
  sweet_gpu_{clk,smmu,gmu,scm,cp,mem,cmddb,core}.c   hardware (plain C)
  sweet_gpu_os_xnu.c, sweet_gpu_os_dma.cpp, SweetAdreno.cpp   XNU/IOKit glue
specs/         A650 register specs (with file:line into Linux 6.17), turnip knl
hosttest/      SoC model + host test of the driver; vktest (Vulkan on the model)
mesa/          turnip "sweet" backend + patch for Mesa 26.2.0 + iOS cross file
userland/
  libsweetgpu/   C wrapper over the UAPI (IOKit on iOS, the model on Linux)
  gputest/       first GPU test on the phone (raw PM4)
  sweetmetal/core/    libsweetmetal_core: C ABI over metal2vulkan (Rust, LGPL)
  sweetmetal/bundle/  SweetMetal.bundle (ObjC, skeleton) + libsweetvk
tools/         mk-xnu-patch.sh, metal-re.sh, llvm-as-compat (mkzap.py is in the top-level tools/)
ci/            build-ios-userland.sh (run by .github/workflows/gpu-userland.yml)
docs/          GPU-DRIVER.md (kernel), METAL.md (Metal)
```

Quick check: `make -C gpu/hosttest check` → `ALL PASSED: 0 failure(s)`.
