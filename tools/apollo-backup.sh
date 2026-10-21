#!/bin/sh
# Back up every partition except userdata from a phone booted into a
# recovery with a root adb shell (e.g. OrangeFox via fastboot boot).
#   tools/apollo-backup.sh [-s SERIAL] [OUTDIR]     (default backup/DATE)
# Only reads from the phone. Each image is checked against a SHA-256 taken
# on the phone. Also saves the partition table of every UFS LUN (primary
# and backup GPT) and the by-name map. Restore one partition with
# `fastboot flash NAME NAME.img`.
set -eu

ADB="adb"
if [ "${1:-}" = "-s" ]; then
	ADB="adb -s ${2:?-s needs a serial}"
	shift 2
fi
out=${1:-backup/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$out"

sh_() { $ADB shell "$@" | tr -d '\r'; }

[ "$(sh_ id -u)" = 0 ] || { echo "error: adb shell is not root (boot a recovery)" >&2; exit 1; }
byname=$(sh_ 'for d in /dev/block/bootdevice/by-name /dev/block/by-name; do [ -d $d ] && echo $d && break; done')
[ -n "$byname" ] || { echo "error: no by-name directory on the phone" >&2; exit 1; }

sh_ "ls -l $byname" > "$out/by-name.txt"

# Partition tables: UFS uses 4 KiB sectors, the GPT header and entries fit
# in the first and last 6 blocks of each LUN.
for lun in $(sh_ 'ls /sys/block | grep "^sd[a-z]$"'); do
	dev=/dev/block/$lun
	blocks=$(( $(sh_ blockdev --getsize64 $dev) / 4096 ))
	$ADB exec-out "dd if=$dev bs=4096 count=6 2>/dev/null" > "$out/gpt-$lun-primary.bin"
	$ADB exec-out "dd if=$dev bs=4096 skip=$((blocks - 6)) count=6 2>/dev/null" > "$out/gpt-$lun-backup.bin"
done

fail=0
for name in $(sh_ "ls $byname"); do
	case "$name" in userdata) continue ;; esac
	dev=$(sh_ "readlink -f $byname/$name")
	size=$(sh_ blockdev --getsize64 "$dev")
	printf '%-24s %12s bytes ... ' "$name" "$size"
	want=$(sh_ "sha256sum $dev" | cut -d' ' -f1)
	$ADB exec-out "cat $dev" > "$out/$name.img"
	got=$(sha256sum "$out/$name.img" | cut -d' ' -f1)
	if [ "$want" = "$got" ] && [ "$(stat -c %s "$out/$name.img")" = "$size" ]; then
		echo ok
		echo "$got  $name.img" >> "$out/SHA256SUMS"
	else
		echo "MISMATCH"
		fail=1
	fi
done

[ "$fail" = 0 ] || { echo "error: some images do not match, run again" >&2; exit 1; }
echo "backup complete: $out"
