#!/bin/bash
# Build the shim and wrap it in a boot.img v2 that mirrors the stock header
# (sweet: PixelOS; apollo: the phone's Android 14 boot.img, gzip kernel).
#   BOARD=sweet|apollo (default sweet)  selects shim/board.h constants
#   DIAG=1|2|3 black-screen diagnostics (shim.c diag_main)
#   TOUCHFW=path  touch probe instead of the hardware probe, with this
#             NT36675 firmware as the ramdisk (apollo)
#   DTB=path  DTB appended to boot.img (default ../dtb/boot_unpacked/dtb,
#             the stock DTB unpacked from the phone's own boot.img)
set -euo pipefail
cd "$(dirname "$0")"
BOARD=${BOARD:-sweet}
DTB=${DTB:-../dtb/boot_unpacked/dtb}
case "$BOARD" in
	sweet)	BOARD_CFLAGS="" ;;
	apollo)	BOARD_CFLAGS="-DBOARD_APOLLO" ;;
	*)	echo "error: unknown BOARD=$BOARD (sweet|apollo)" >&2; exit 1 ;;
esac
# Header values of each phone's stock boot.img. apollo's is an Android 14
# image with a gzip kernel.
case "$BOARD" in
	apollo)	GZIP_KERNEL=${GZIP_KERNEL:-1}; OS_VERSION=${OS_VERSION:-14.0.0}; OS_PATCH=${OS_PATCH:-2024-04} ;;
	*)	GZIP_KERNEL=${GZIP_KERNEL:-0}; OS_VERSION=${OS_VERSION:-17.0.0}; OS_PATCH=${OS_PATCH:-2035-06} ;;
esac
[ -f "$DTB" ] || { echo "error: DTB $DTB not found (tools/unpack_bootv2.py BOOT.img dtb/boot_unpacked)" >&2; exit 1; }
# DIAG=1|2|3: black-screen diagnostics (see diag_main in shim.c)
[ -n "${DIAG:-}" ] && BOARD_CFLAGS="$BOARD_CFLAGS -DDIAG=$DIAG"
[ -n "${TOUCHFW:-}" ] && BOARD_CFLAGS="$BOARD_CFLAGS -DTOUCH_PROBE"
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra $BOARD_CFLAGS"
mkdir -p out
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c touch.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(aarch64-linux-gnu-readelf -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o out/touch.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o out/touch.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$((0x$(aarch64-linux-gnu-nm out/shim.elf | awk '$3=="_end"{print $1}')))	# no gawk needed
truncate -s "$size" out/Image
KERNEL=out/Image
if [ "$GZIP_KERNEL" = 1 ]; then
	gzip -9 -n -c out/Image > out/Image.gz
	KERNEL=out/Image.gz
fi
# Ramdisk: an xnu payload (tools/mkpayload.py) if given as $1, else an empty
# newc cpio (ABL dislikes a zero-size ramdisk on some builds) -> diagnostics.
if [ -n "${TOUCHFW:-}" ]; then
	cp "$TOUCHFW" out/ramdisk.bin
elif [ -n "${1:-}" ]; then
	cp "$1" out/ramdisk.bin
else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
mkbootimg --header_version 2 --kernel "$KERNEL" --ramdisk out/ramdisk.bin \
	--dtb "$DTB" --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
	--tags_offset 0x100 --dtb_offset 0x1f00000 --pagesize 4096 \
	--os_version "$OS_VERSION" --os_patch_level "$OS_PATCH" \
	--cmdline "console=null" -o out/shim-boot.img
# Host render of the console layout.
gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
ls -l "$KERNEL" out/shim-boot.img
