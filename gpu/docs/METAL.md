# Metal → Vulkan on apollo: analysis, architecture, current state

Date: 2026-10-21. Code: `gpu/userland/sweetmetal/` (C ABI + bundle),
`gpu/mesa/` (turnip); the kernel side is in `GPU-DRIVER.md`.

## 1. Why Metal, and why through Vulkan

In iOS 16 the whole UI is drawn by the CoreAnimation render server in
`backboardd`. It picks a renderer in this order: Metal
(`CAMetalContextCreate`), then GLES, then the software `CA::OGL::SW`. Right
now sweet/apollo run the software renderer, because there is no Metal device.
GLES on the A11 also runs on top of Metal (AppleMetalGLRenderer). So **one
Metal driver gives us the UI, GLES apps and Metal games**.

There is no point writing Metal "directly on Adreno": A6xx command generation
and a shader compiler already exist in Mesa turnip (Vulkan 1.3, conformant)
and ir3. Metal → Vulkan is the shortest path, and it has already been proven:
the NullMoth driver for NVIDIA on macOS 15 is built the same way (Metal
plugin → AIR→SPIR-V based on metal2vulkan → NVK → its own kexts), and
WindowServer composites through it.

## 2. How Metal finds the driver

Facts from macOS (from the NullMoth sources, which are PolyForm
Noncommercial: only facts about Apple's interface were taken from there, no
code):

1. The accelerator service in the IORegistry carries `MetalPluginName` (the
   bundle name) and `MetalPluginClassName` (the device class). On macOS,
   without `MetalPluginName` Metal does not load the bundle (NullMoth
   issue #106).
2. Metal.framework loads the bundle and creates the device with
   **`-[<MetalPluginClassName> initWithAcceleratorPort:(mach_port_t)]`**.
3. NullMoth's device class is a **subclass of the private
   `MTLIOAccelDevice`**, and it calls `[super initWithAcceleratorPort:]`. The
   other classes (buffers, textures, queues, encoders) are `NSObject`s with
   Metal protocols, plus a table mapping them to the `MTLIOAccel*` classes
   (`NVMTLForward.m`). So something in the system looks at the "ancestry" of
   the objects.

**The main problem for us.** `-[MTLIOAccelDevice initWithAcceleratorPort:]`
talks to the kernel over the IOAccelerator family ABI (IOAcceleratorFamily2 /
IOGPU: user clients for the device, shared memory, queues). On macOS NullMoth
inherits that kext from Apple. Our XNU, built from source, does not have this
family: it is a closed Apple kext.

Options, in order of increasing cost:

| Option | Idea | Risk |
|---|---|---|
| **A (done in the skeleton)** | `SweetMetalDevice : NSObject <MTLDevice>` with its own `-initWithAcceleratorPort:`, no `MTLIOAccelDevice` | if QuartzCore/Metal checks the class or reaches into `MTLIOAccel*` ivars, it crashes. That shows up on the first run |
| B | subclass `MTLIOAccelDevice`, but initialize through `-[NSObject init]` (bypassing super) | the parent's ivars are zero: parent methods that still get called have to be overridden |
| C | emulate the needed part of the IOAccelerator/IOGPU ABI in the kernel | a large amount of closed ABI; only worth doing if RE shows there is no way around it |

What to check on iOS 16 (section 4) before writing more: whether the
mechanism is the same (`MetalPluginName` → `initWithAcceleratorPort:`), what
class the real `AGXMetalA11` has, and who checks the device class.

## 3. Architecture

```
QuartzCore / UIKit / games
      │  objc_msgSend through the MTLDevice, MTLCommandBuffer, ... protocols
SweetMetal.bundle            gpu/userland/sweetmetal/bundle/SweetMetal.m   (ObjC, our code)
  SweetMetalDevice, ...Buffer, ...Texture, ...CommandQueue/Buffer, ...Encoder
  each class holds one smtl_* handle and forwards the call;
  unknown selector → "not implemented" log and nil (not a crash)
      │  C ABI (include/sweetmetal.h)
libsweetmetal_core.dylib     gpu/userland/sweetmetal/core   (Rust, LGPL-3.0+)
  over metal2vulkan-runtime: the Metal object model on Vulkan, hazard tracking,
  argument buffers, IOSurface textures; metal2vulkan: AIR → SPIR-V
      │  Vulkan 1.3 (vkGetInstanceProcAddr from libsweetvk)
libsweetvk.dylib             15 lines: vkGetInstanceProcAddr → vk_icdGetInstanceProcAddr
libvulkan_freedreno.dylib    Mesa turnip + the sweet backend (gpu/mesa/0001-…patch)
      │  libsweetgpu → IOConnectCallStructMethod
SweetAdreno (XNU)            GPU-DRIVER.md
```

**Why metal2vulkan** (steelbrain/metal2vulkan, commit `a2c440b`,
2026-10-19):

- It is not just a translator but the whole Metal object model on Vulkan
  (`metal2vulkan-runtime`). It has devices, queues, encoders, heaps, events,
  argument buffers, ICBs and even Apple's private selectors
  (fill/invalidate). Our ObjC layer becomes a thin adapter.
- Render, compute, tessellation, tile and mesh pipelines. If a function
  cannot be executed exactly, the result is a typed refusal, not a silently
  wrong result. Approximations are recorded in "precision notes".
- It has `newTextureWithDescriptor:iosurface:plane:` (a texture over a buffer
  that stands for an IOSurface), adoption of a foreign `VkDevice`/`VkBuffer`,
  and loading Vulkan through a library of your choice. For iOS with no Vulkan
  loader that is exactly what we need.
- License GPL-2.0+ or LGPL-3.0+. We keep it in a separate dylib
  (`libsweetmetal_core`), and the bundle links to it dynamically.
- The project is active, and its translator is already used in a working
  driver (NullMoth).
- Alternatives: Indium/Iridium (darlinghq, 0BSD), last commit in 2023, less
  coverage. Our own translator would be years of work.

## 4. Reverse engineering still to be done on the PC

Script `gpu/tools/metal-re.sh` (needs `ipsw` and the iPhone X 20H392 dyld
shared cache from `research/`, not from git):

```sh
gpu/tools/metal-re.sh research/dsc/dyld_shared_cache_arm64 \
    research/ipsw-x/kernelcache.release.iphone10b out/metal-re
```

| # | Question | What it decides |
|---|---|---|
| RE-1/2 | Which `IOServiceMatching` and which keys `MTLCreateSystemDefaultDevice` reads on iOS 16 | the class/property names SweetAdreno publishes (currently `MetalPluginName=SweetMetal`, `MetalPluginClassName=SweetMetalDevice` with `sweetmetal=1`). If Metal looks the service up by class (e.g. `IOAcceleratorES`), the kernel needs a class with that name |
| RE-3 | Superclass of the main `AGXMetalA11` class and the init selector | option A/B/C from §2 |
| RE-4 | Selectors QuartzCore sends to the device and resources | the order in which to implement methods |
| RE-5 | Class checks / `MTLIOAccel*` ivar access in QuartzCore, CoreImage, UIKitCore | whether we can stay on option A |
| RE-6 | QuartzCore metallibs → `metal2vulkan --profile gpu/hosttest/a650-turnip-profile.json` | how many system shaders translate without refusals |
| RE-7 | What the real AGX service publishes in the kernelcache | missing properties |

## 5. Individual paths

### 5.1 Shaders

Metal on the device receives AIR (LLVM bitcode from a `.metallib`).
CoreAnimation's system shaders are compiled on the device by
`MTLCompilerService`. In our stack metal2vulkan translates AIR in-process,
then turnip compiles SPIR-V → NIR → ir3. Translated shaders are cached on
disk (the directory is passed to `smtl_device_create`; on iOS it is the
process's tmp/cache container).

### 5.2 Memory

The GPU and CPU share DDR (unified memory). Metal `Shared` buffers are turnip
BOs that the kernel maps into the task at allocation time (`MEM_ALLOC`).
Write-combine by default; cached memory types need `DC CVAC/CIVAC` in
userspace (turnip does this itself). It still has to be checked whether our
XNU allows these instructions at EL0 (`SCTLR_EL1.UCI`); there is a test in
turnip-knl.md §4.2-8.

### 5.3 IOSurface, the UI path

CoreAnimation renders layers into IOSurfaces. What already exists:

- kernel: `IMPORT_SURFACE` (by SweetSurface id) maps the surface pages into
  the GPU;
- metal2vulkan: a texture over a buffer that stands for the surface.

What is left (the next task): in the turnip backend, `bo_init_dmabuf` has to
accept a SweetSurface id → `sgpu_import_surface` → `tu_bo`. Then SweetMetal
creates a `VkBuffer` with that memory and hands it to metal2vulkan through
`Device::new_buffer_with_vk_buffer`. For now the skeleton has an
intermediate buffer and a log message in that spot.

### 5.4 Drawables and presentation

`CAMetalLayer.nextDrawable` returns an IOSurface from its pool;
`presentDrawable:` schedules presentation after the command buffer
completes. The presentation itself (a swap in IOMobileFramebuffer) is done by
QuartzCore, and our AppleCLCD already supports IOSurface swaps. What the
driver needs to provide is correct completion handlers and textures over
IOSurfaces (5.3).

### 5.5 Which GPU families to claim

Start with `Apple1–3` / `Common1–2` (`-supportsFamily:` in the skeleton) and
raise it as the implementation grows. If we claim `Apple4` (A11), games will
start using tile shaders and imageblocks, for which metal2vulkan gives
refusals or emulation.

## 6. What has been tested (Linux, no phone)

The whole stack below Metal runs on a PC on top of the SoC model
(`gpu/hosttest`, `LD_PRELOAD=libsgpu_host.so SGPU_HOST_AUTOBOOT=1`):

| Check | Result |
|---|---|
| `vulkaninfo` through the real Vulkan loader | Turnip Adreno (TM) 650, Vulkan 1.3.354, Mesa 26.2.0; device profile saved in `hosttest/a650-turnip-profile.json` |
| `metal2vulkan` CLI: AIR → SPIR-V for the A650 profile | compute (`scale_and_offset`) and vertex+fragment (`colored_vertex/colored_fragment`): valid Vulkan 1.2/1.3 SPIR-V |
| metal2vulkan-runtime `compute_dispatch`, `render_to_texture` | Metal API → AIR translation → ir3 → command buffers → our driver → model CP: 397 and 510 packets, **0 model errors** |
| `sweetmetal-core` (C ABI) `smtl_test` | compute, render to a private texture + blit, render to a texture "over an IOSurface", errors reported cleanly; 941 packets, 0 errors |
| no Vulkan loader (as on iOS) | `smtl_device_create("libsweetvk.so")` opens turnip directly |

**Limitation:** the model has no shader cores, so numbers and pixels are not
computed (`output[1] = 0`; on hardware it will be 3). That can only be checked
on the phone. Before hardware, the APIs, shader translation, ir3 compilation,
command streams and the path through the kernel have been tested.

What metal2vulkan reported for A650/turnip (for judging Metal accuracy):
- `float-tininess-after-rounding` and `native-fma`, `native-nan-encoding`:
  small differences in denormals, FMA and NaN encoding; there is an exact
  mode, `FloatMathsPolicy::Exact`.
- `split-dispatch`: a grid that is not a multiple of the threadgroup becomes
  several dispatches.
- `shader-color-conversion`: ~35 instructions per channel for Metal-style
  rounding.
- `driver-pipeline-cache-kept`: turnip has no `VK_KHR_pipeline_binary`.

Found along the way: with a debug build of turnip, the metal2vulkan "primer"
draws with zero scissors and hits an assert in `tu6_emit_scissor`. A release
build (`-Db_ndebug=true`, which is how the iOS script builds it) does not
have this. Worth reporting to metal2vulkan/Mesa.

## 7. Turning it on safely

While `sweetmetal=0` (the default), the kernel does not publish the plugin
properties, and QuartzCore stays on the working software renderer. The order
for turning it on:

1. `sweetgpu=8`, `gputest`: the kernel and GPU are fine.
2. Our own test binary using the Metal API, **without** `sweetmetal=1`: the
   property is global, and with it backboardd would switch to Metal too. So
   the test program loads the bundle itself (`dlopen` +
   `[[SweetMetalDevice alloc] initWithAcceleratorPort:0]`) and draws through
   the Metal protocols.
3. Then `sweetmetal=1` and a single app, then backboardd. The "not
   implemented" log from `SMBase` is the list of what is left to write.

## 8. Next steps (by priority)

1. RE-1…RE-5 (the script is ready) → decide on A/B/C.
2. IOSurface import (5.3): turnip `bo_init_dmabuf` +
   `new_buffer_with_vk_buffer`.
3. First CI run of `gpu-userland.yml` on macOS: building turnip for iOS,
   compiling the bundle (the ObjC has not been compiled yet, as there is no
   iOS SDK here).
4. On the phone: `smtl_test` (real pixels), then a test through
   Metal.framework.
5. Methods from the "not implemented" log for QuartzCore: blending, vertex
   descriptors, depth/stencil in the render pipeline, samplers, MSAA.

## 9. Licenses

- Our code (bundle, C ABI wrapper, kernel, turnip backend): same as the
  project.
- metal2vulkan: GPL-2.0+ OR LGPL-3.0+. `libsweetmetal_core` is a separate
  dynamic library (LGPL); the bundle does not embed it statically.
- Mesa: MIT. The `tu_knl_sweet.cc` backend is derived from `tu_knl_kgsl.cc`
  (MIT, Google copyright kept).
- NullMoth: PolyForm Noncommercial. No code used, only facts about Apple's
  interface.
