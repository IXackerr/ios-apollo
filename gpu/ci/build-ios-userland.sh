#!/usr/bin/env bash
# Build the GPU userland for iOS 16 arm64 on macOS (Xcode 15, as the kernel CI):
#   mesa   libvulkan_freedreno.dylib (Mesa turnip + sweet backend)
#   tools  libsweetvk.dylib, vktest (hosttest/vktest.c for the phone)
#   metal  libsweetmetal_core.dylib (metal2vulkan runtime, LGPL), SweetMetalImpl.bundle
#          (the driver) and SweetMetal.bundle (the loader installed on the root)
# PARTS selects them (default: all three, in this order).
# Output: $OUT/  (default build-ios/out).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)          # gpu/
OUT=${OUT:-$ROOT/build-ios/out}
WORK=${WORK:-$ROOT/build-ios}
MESA_REV=${MESA_REV:-mesa-26.2.0}
SDK=$(xcrun -sdk iphoneos --show-sdk-path)
CC="xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0"
PARTS=${PARTS:-mesa tools metal}
REPO=$(cd "$ROOT/.." && pwd)
mkdir -p "$OUT" "$WORK"
want() { case " $PARTS " in *" $1 "*) return 0;; esac; return 1; }

# ---- Mesa turnip with the sweet backend ----
if want mesa; then
if [ ! -d "$WORK/mesa" ]; then
  git clone --depth 1 --branch "$MESA_REV" https://github.com/chaotic-cx/mesa-mirror.git "$WORK/mesa"
  for p in "$ROOT"/mesa/*.patch; do git -C "$WORK/mesa" apply "$p"; done
fi
python3 -m venv "$WORK/venv"
. "$WORK/venv/bin/activate"
pip install -q meson ninja mako pyyaml packaging
command -v glslangValidator >/dev/null || brew install glslang
# Xcode's bison 2.3 is too old for ir3_parser.y
brew list bison >/dev/null 2>&1 || brew install bison
brew list flex >/dev/null 2>&1 || brew install flex
export PATH="$(brew --prefix bison)/bin:$(brew --prefix flex)/bin:$PATH"
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
fi

# ---- libsweetvk (vkGetInstanceProcAddr -> turnip), libsweetgpu, vktest ----
if want tools || want metal; then
$CC -dynamiclib -I"$WORK/mesa/include" -o "$OUT/libsweetvk.dylib" \
  "$ROOT/userland/sweetmetal/bundle/libsweetvk.c" -L"$OUT" -lvulkan_freedreno \
  -install_name @rpath/libsweetvk.dylib -Wl,-rpath,@loader_path
$CC -c -I"$ROOT/kernel" -o "$WORK/sweetgpu.o" "$ROOT/userland/libsweetgpu/sweetgpu.c"
$CC -I"$WORK/mesa/include" -I"$REPO/ios/sweetrun" -o "$OUT/vktest" \
  "$ROOT/hosttest/vktest.c" "$REPO/ios/sweetrun/sweetlog.c" "$REPO/ios/sweetrun/sweetfile.c" -framework IOKit -framework CoreFoundation -lz
fi

# ---- sweetmetal-core (Rust, LGPL: separate dylib; metal2vulkan pinned in Cargo.toml) ----
if want metal; then
rustup target add aarch64-apple-ios
(cd "$ROOT/userland/sweetmetal/core" && \
  CARGO_TARGET_DIR="$WORK/cargo" cargo build --release --ignore-rust-version --target aarch64-apple-ios)
cp "$WORK/cargo/aarch64-apple-ios/release/libsweetmetal_core.dylib" "$OUT/"
install_name_tool -id @rpath/libsweetmetal_core.dylib "$OUT/libsweetmetal_core.dylib"

# ---- SweetMetalImpl.bundle: the driver, from the boot image (mkadt.py --metal-file) ----
# Built with the device class renamed: SweetMetalDevice is the loader's.
I="$OUT/SweetMetalImpl.bundle"
mkdir -p "$I"
sed -e 's|<string>SweetMetal</string>|<string>SweetMetalImpl</string>|' \
    -e 's|org.sweet.SweetMetal<|org.sweet.SweetMetalImpl<|' \
    -e 's|<string>SweetMetalDevice</string>|<string>SweetMetalDeviceImpl</string>|' \
    "$ROOT/userland/sweetmetal/bundle/Info.plist" > "$I/Info.plist"
$CC -bundle -fobjc-arc -fmodules -DSweetMetalDevice=SweetMetalDeviceImpl \
  -I"$ROOT/userland/sweetmetal/core/include" \
  -I"$ROOT/userland/libsweetgpu" -I"$ROOT/kernel" -o "$I/SweetMetalImpl" \
  "$ROOT/userland/sweetmetal/bundle/SweetMetal.m" "$ROOT/userland/sweetmetal/bundle/profile.c" \
  "$WORK/sweetgpu.o" \
  -L"$OUT" -lsweetmetal_core -framework Foundation -framework Metal -framework IOSurface \
  '-Wl,-U,_OBJC_CLASS_$__MTLDevice' '-Wl,-U,_OBJC_METACLASS_$__MTLDevice' \
  -framework IOKit -framework CoreFoundation -Wl,-rpath,@loader_path
cp "$OUT"/libsweetmetal_core.dylib "$OUT"/libsweetvk.dylib "$OUT"/libvulkan_freedreno.dylib "$I/"

# ---- SweetMetal.bundle: the loader installed on the root ----
B="$OUT/SweetMetal.bundle"
mkdir -p "$B"
cp "$ROOT/userland/sweetmetal/bundle/Info.plist" "$B/"
$CC -bundle -fobjc-arc -fmodules -I"$REPO/ios/sweetrun" -o "$B/SweetMetal" \
  "$ROOT/userland/sweetmetal/loader/SweetMetalLoader.m" "$REPO/ios/sweetrun/sweetfile.c" \
  '-Wl,-U,_OBJC_CLASS_$__MTLDevice' '-Wl,-U,_OBJC_METACLASS_$__MTLDevice' \
  -framework Foundation -framework IOKit -framework CoreFoundation -lz
fi

# The iOS root is unsigned-friendly (permissive AMFI), but sign ad hoc anyway.
for f in "$OUT"/*.dylib "$OUT"/vktest "$OUT"/SweetMetal.bundle/SweetMetal \
    "$OUT"/SweetMetalImpl.bundle/SweetMetalImpl "$OUT"/SweetMetalImpl.bundle/*.dylib; do
  [ -f "$f" ] && codesign -f -s - "$f"
done
echo "built into $OUT ($PARTS)"
