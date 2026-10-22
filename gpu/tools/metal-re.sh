#!/usr/bin/env bash
# metal-re.sh - answer the open Metal-plugin questions (docs/METAL.md, section 4)
# from the iPhone X iOS 16.7.x (20H392) dyld shared cache and kernelcache.
#
# Run on the PC that has the extracted IPSW (not in git; Apple files):
#   gpu/tools/metal-re.sh research/dsc/dyld_shared_cache_arm64 \
#       research/ipsw-x/kernelcache.release.iphone10b  out/metal-re
#
# Needs: blacktop/ipsw (brew install ipsw / go install), strings, grep.
# Every step writes a file into OUT and prints a one-line verdict; a failing
# step prints the command to retry by hand (ipsw flags differ by version).
set -u
DSC=${1:?dyld_shared_cache_arm64}
KC=${2:-}
OUT=${3:-metal-re}
mkdir -p "$OUT"

step() { echo; echo "== $*"; }
try() { local f=$1; shift; if "$@" > "$OUT/$f" 2>&1; then echo "   -> $OUT/$f ($(wc -l < "$OUT/$f") lines)"; else echo "   !! failed: $*"; fi; }

step "RE-1  Which images mention the plugin keys (who reads MetalPluginName)?"
for key in MetalPluginName MetalPluginClassName MetalStatisticsName IOGLESBundleName \
           initWithAcceleratorPort: MTLIOAccelDevice IOGPUMetalDevice AGXAccelerator \
           IOAcceleratorES IOGPU; do
  try "str-$key.txt" ipsw dyld str "$DSC" "$key"
done

step "RE-2  How MTLCreateSystemDefaultDevice finds and instantiates the driver"
try disass-MTLCreateSystemDefaultDevice.txt ipsw dyld disass "$DSC" --image Metal --symbol _MTLCreateSystemDefaultDevice
try disass-MTLCopyAllDevices.txt ipsw dyld disass "$DSC" --image Metal --symbol _MTLCopyAllDevices
# The IOServiceMatching() class name and the property keys it reads appear as
# cstring refs in these functions; also dump the whole Metal image's strings.
try strings-Metal.txt ipsw dyld str "$DSC" --image Metal ""

step "RE-3  Class layout: device base classes and the driver's principal class"
try classdump-Metal.txt ipsw class-dump "$DSC" Metal
try classdump-IOGPU.txt ipsw class-dump "$DSC" IOGPU
try classdump-AGXMetalA11.txt ipsw class-dump "$DSC" AGXMetalA11
grep -nE "@interface (MTLIOAccel|IOGPUMetal|_MTL|AGX)[A-Za-z0-9]*Device[A-Za-z0-9]* *:" \
  "$OUT"/classdump-*.txt > "$OUT/device-superclasses.txt" 2>/dev/null
echo "   device superclass chain: $OUT/device-superclasses.txt"
grep -nE "initWithAcceleratorPort|initWithDevice|initWithIOGPUDevice|acceleratorPort" \
  "$OUT"/classdump-*.txt > "$OUT/init-selectors.txt" 2>/dev/null
echo "   init selectors: $OUT/init-selectors.txt"

step "RE-4  What QuartzCore sends to the Metal device / resources (selector set to implement)"
try classdump-QuartzCore.txt ipsw class-dump "$DSC" QuartzCore
try objc-sel-QuartzCore.txt ipsw dyld objc --sel "$DSC" --image QuartzCore
grep -oE "(new|set|copy|draw|dispatch|render|blit|compute|present|add|commit|wait|supports|make)[A-Za-z0-9:]*" \
  "$OUT/objc-sel-QuartzCore.txt" 2>/dev/null | sort -u > "$OUT/quartzcore-selectors.txt"
echo "   candidate selectors: $OUT/quartzcore-selectors.txt"
# CA::OGL::MetalContext is C++: its Metal calls are objc_msgSend with these selrefs.
try str-MetalContext.txt ipsw dyld str "$DSC" --image QuartzCore "MetalContext"

step "RE-5  Does anything check the device class (isKindOfClass:/IOAccel ivars)?"
try str-MTLIOAccel-in-QuartzCore.txt ipsw dyld str "$DSC" --image QuartzCore "MTLIOAccel"
try str-MTLIOAccel-in-CoreImage.txt ipsw dyld str "$DSC" --image CoreImage "MTLIOAccel"
try str-MTLIOAccel-in-UIKitCore.txt ipsw dyld str "$DSC" --image UIKitCore "MTLIOAccel"

step "RE-6  CoreAnimation shaders: AIR metallibs we must translate"
echo "   On the extracted root filesystem (not the cache):"
echo "   find <iosroot>/System/Library/Frameworks/QuartzCore.framework -name '*.metallib'"
echo "   then for each: metal2vulkan <lib>.metallib /tmp/x.spv --profile hosttest/a650-turnip-profile.json --function <name>"
echo "   (list functions: metal2vulkan <lib>.metallib --list, or strings | grep -E '^[a-z_]+$')"

if [ -n "$KC" ]; then
  step "RE-7  Kernel side: what the real AGX service publishes and which user clients Metal opens"
  strings -a "$KC" | grep -E "MetalPluginName|MetalPluginClassName|IOGLESBundleName|IOClass|AGXMetal|IOGPUDevice|IOAccel" \
    | sort -u > "$OUT/kernelcache-strings.txt"
  echo "   -> $OUT/kernelcache-strings.txt"
  try kernelcache-kexts.txt ipsw kernel kexts "$KC"
fi

cat > "$OUT/README.txt" <<'EOF'
What to look for (docs/METAL.md section 4):
 1. RE-1/RE-2: the IOServiceMatching class name Metal uses on iOS 16 and which
    property names it reads (MetalPluginName / MetalPluginClassName?). The kernel
    publishes those on SweetAdreno with sweetmetal=1 (patch 0083).
 2. RE-3: the principal class of AGXMetalA11 and its superclass chain
    (AGXA11Device : ... : MTLIOAccelDevice or IOGPUMetalDevice?), and the init
    selector Metal calls (initWithAcceleratorPort: on macOS).
 3. RE-4: the selector list -> the methods SweetMetal must implement first.
 4. RE-5: any class identity checks / ivar access on MTLIOAccel* objects by
    QuartzCore -> decides whether SweetMetalDevice can stay an NSObject.
 5. RE-6: how many QuartzCore metallib functions translate for the A650 profile.
EOF
echo; echo "done: $OUT/README.txt lists what each output answers"
