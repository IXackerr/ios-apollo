#!/usr/bin/env bash
# Build the GPU userland for iOS 16 arm64 on macOS (Xcode 15, as the kernel CI):
#   libvulkan_freedreno.dylib (Mesa turnip + sweet backend)
#   libsweetvk.dylib, libsweetgpu (static), gputest
#   libsweetmetal_core.dylib (metal2vulkan runtime, LGPL) and SweetMetal.bundle
# Output: $OUT/  (default build-ios/out). Status: written for the handoff, not
# yet run on a Mac - expect to fix flags on the first CI run.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)          # gpu/
OUT=${OUT:-$ROOT/build-ios/out}
WORK=${WORK:-$ROOT/build-ios}
MESA_REV=${MESA_REV:-mesa-26.2.0}
SDK=$(xcrun -sdk iphoneos --show-sdk-path)
CC="xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0"
mkdir -p "$OUT" "$WORK"

# ---- Mesa turnip with the sweet backend ----
if [ ! -d "$WORK/mesa" ]; then
  git clone --depth 1 --branch "$MESA_REV" https://github.com/chaotic-cx/mesa-mirror.git "$WORK/mesa"
  git -C "$WORK/mesa" apply "$ROOT/mesa/0001-turnip-sweet-kernel-backend.patch"
fi
pip3 install --user -q meson mako pyyaml packaging
command -v glslangValidator >/dev/null || brew install glslang
meson setup "$WORK/mesa-build" "$WORK/mesa" --cross-file "$ROOT/mesa/ios-arm64.ini" \
  --buildtype=release -Db_ndebug=true \
  -Dvulkan-drivers=freedreno -Dfreedreno-kmds=sweet -Dgallium-drivers= -Dplatforms= \
  -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled \
  -Dgbm=disabled -Dglvnd=disabled -Dllvm=disabled -Dxmlconfig=disabled -Dexpat=disabled \
  -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dzstd=disabled \
  -Dperfetto=false -Dbuild-tests=false -Dtools= -Dvulkan-layers= -Dvideo-codecs= \
  -Dshader-cache=disabled -Dtu-build-id=5713e7c0ffee0650 --reconfigure
ninja -C "$WORK/mesa-build" src/freedreno/vulkan/libvulkan_freedreno.dylib
cp "$WORK/mesa-build/src/freedreno/vulkan/libvulkan_freedreno.dylib" "$OUT/"
install_name_tool -id @rpath/libvulkan_freedreno.dylib "$OUT/libvulkan_freedreno.dylib"

# ---- libsweetvk (vkGetInstanceProcAddr -> turnip), libsweetgpu, gputest ----
$CC -dynamiclib -I"$WORK/mesa/include" -o "$OUT/libsweetvk.dylib" \
  "$ROOT/userland/sweetmetal/bundle/libsweetvk.c" -L"$OUT" -lvulkan_freedreno \
  -install_name @rpath/libsweetvk.dylib -Wl,-rpath,@loader_path
$CC -c -I"$ROOT/kernel" -o "$WORK/sweetgpu.o" "$ROOT/userland/libsweetgpu/sweetgpu.c"
$CC -I"$ROOT/kernel" -I"$ROOT/userland/libsweetgpu" -o "$OUT/gputest" \
  "$ROOT/userland/gputest/gputest.c" "$WORK/sweetgpu.o" -framework IOKit -framework CoreFoundation

# ---- sweetmetal-core (Rust, LGPL: separate dylib; metal2vulkan pinned in Cargo.toml) ----
rustup target add aarch64-apple-ios
(cd "$ROOT/userland/sweetmetal/core" && \
  CARGO_TARGET_DIR="$WORK/cargo" cargo build --release --ignore-rust-version --target aarch64-apple-ios)
cp "$WORK/cargo/aarch64-apple-ios/release/libsweetmetal_core.dylib" "$OUT/"
install_name_tool -id @rpath/libsweetmetal_core.dylib "$OUT/libsweetmetal_core.dylib"

# ---- SweetMetal.bundle ----
B="$OUT/SweetMetal.bundle"
mkdir -p "$B"
cp "$ROOT/userland/sweetmetal/bundle/Info.plist" "$B/"
$CC -bundle -fobjc-arc -fmodules -I"$ROOT/userland/sweetmetal/core/include" \
  -I"$ROOT/userland/libsweetgpu" -I"$ROOT/kernel" -o "$B/SweetMetal" \
  "$ROOT/userland/sweetmetal/bundle/SweetMetal.m" "$WORK/sweetgpu.o" \
  -L"$OUT" -lsweetmetal_core -framework Foundation -framework Metal -framework IOSurface \
  -framework IOKit -framework CoreFoundation -Wl,-rpath,@loader_path
cp "$OUT"/libsweetmetal_core.dylib "$OUT"/libsweetvk.dylib "$OUT"/libvulkan_freedreno.dylib "$B/"

# The iOS root is unsigned-friendly (permissive AMFI), but sign ad hoc anyway.
for f in "$OUT"/*.dylib "$OUT/gputest" "$B/SweetMetal" "$B"/*.dylib; do codesign -f -s - "$f"; done
echo "built into $OUT: install SweetMetal.bundle to /System/Library/Extensions/ (docs/HANDOFF-GPU.md)"
