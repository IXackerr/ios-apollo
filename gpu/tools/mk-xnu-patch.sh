#!/usr/bin/env bash
# mk-xnu-patch.sh XNU_TREE OUT.patch
# Regenerate xnu/patches/0083-apollo-adreno-gpu.patch from gpu/kernel.
# XNU_TREE: xnu-8796.141.3 with xnu/patches/0001..0082 applied and committed
# (git clone --branch xnu-8796.141.3 apple-oss-distributions/xnu; git apply each
# patch in order; 0031/0041 need the .sh staging scripts and may be skipped,
# exactly as darwin-xnu-build does).
set -euo pipefail
X=$1; OUT=$2
K=$(cd "$(dirname "$0")/../kernel" && pwd)
cd "$X"
git diff --quiet && git diff --cached --quiet || { echo "$X: tree not clean"; exit 1; }
cp "$K/sweet_gpu.h" "$K/sweet_gpu_uapi.h" bsd/dev/
for f in clk cmddb core cp gmu mem scm smmu os_xnu; do cp "$K/sweet_gpu_$f.c" bsd/dev/; done
cp "$K/SweetAdreno.cpp" iokit/Kernel/arm/SweetAdreno.cpp
cp "$K/sweet_gpu_os_dma.cpp" iokit/Kernel/arm/SweetAdrenoDMA.cpp
python3 - <<'PY'
import re
def edit(p, anchor, add, after=True):
    s = open(p).read()
    if add in s:
        return
    assert anchor in s, (p, anchor)
    s = s.replace(anchor, anchor + add if after else add + anchor, 1)
    open(p, 'w').write(s)
objs = ["clk", "cmddb", "core", "cp", "gmu", "mem", "scm", "smmu", "os_xnu"]
edit('bsd/conf/files', "bsd/dev/sweet_usb.c\t\t\tstandard\n",
     ''.join(f"bsd/dev/sweet_gpu_{o}.c\t\t\tstandard\n" for o in objs))
edit('bsd/conf/Makefile.template', "$(foreach o,$(SWEET_HFS_OBJS),",
     "# SweetAdreno GPU driver (patch 0083): MMIO/DMA buffers are raw pointers;\n"
     "# newer clangs extend -Wunsafe-buffer-usage (part of -Weverything) to C.\n"
     "SWEET_GPU_OBJS = " + ' '.join(f"sweet_gpu_{o}.o" for o in objs) + "\n"
     "$(foreach o,$(SWEET_GPU_OBJS),$(eval $(o)_CWARNFLAGS_ADD += -Wno-unsafe-buffer-usage))\n",
     after=False)
edit('iokit/conf/files.arm64',
     "iokit/Kernel/arm/SweetBattery.cpp                                optional iokitcpp\n",
     "iokit/Kernel/arm/SweetAdreno.cpp                                 optional iokitcpp\n"
     "iokit/Kernel/arm/SweetAdrenoDMA.cpp                              optional iokitcpp\n")
edit('iokit/conf/Makefile.template', "IODataQueue.cpo_CXXWARNFLAGS_ADD += -Wno-cast-align\n",
     "SweetAdreno.cpo_CXXWARNFLAGS_ADD += -Wno-unsafe-buffer-usage -Wno-cast-align\n"
     "SweetAdrenoDMA.cpo_CXXWARNFLAGS_ADD += -Wno-unsafe-buffer-usage\n", after=False)
edit('iokit/KernelConfigTables.cpp', """    "     'IOMatchCategory' = SweetBattery;"
    "   },"
""", """    "   {"
    "     'IOClass'         = SweetAdreno;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = SweetAdreno;"
    "   },"
""")
PY
git add -A
git diff --cached > "$OUT"
git reset -q --hard
git apply --check "$OUT"
echo "$OUT: $(grep -c '^+++ ' "$OUT") files, applies cleanly on top of 0001-0082"
