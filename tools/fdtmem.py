#!/usr/bin/env python3
"""Pick the RAM window XNU may own, from a phone's live FDT.

  tools/fdtmem.py LIVE.dtb [--max-size HEX] [--align HEX] [--all]

LIVE.dtb is the device tree as ABL handed it to the kernel, with the real
memory banks filled in: from a running Android or a temporarily booted
recovery, `adb pull /sys/firmware/fdt live.dtb` (needs root). The stock DTB
inside boot.img is NOT enough: its /memory reg is empty until ABL patches it.

Prints the memory banks, every fixed carve-out (/memreserve/ entries and
/reserved-memory children with a reg), the free windows left over and the
suggested --phys-base/--mem-size for tools/mkadt.py.

Policy (from the sweet port, see NOTES.md):
  - prefer windows at or above 4 GiB: ABL loads the shim, the DTB and the
    ramdisk in the low bank, and the shim must not copy the kernel over them;
  - cap the size at --max-size (default 0xfcc00000, the ~3.95 GiB window that
    is known to boot on sweet); raise it only after a successful boot;
  - align the base up to --align (default 32 MiB, the kernel's VA alignment).
Dynamic reserved-memory pools (size without reg) are ignored: Linux places
them at runtime, nothing occupies them before our kernel starts.
"""
import argparse, struct, sys

FDT_MAGIC = 0xd00dfeed
BEGIN_NODE, END_NODE, PROP, NOP, END = 1, 2, 3, 4, 9


class Node:
    def __init__(self, name):
        self.name, self.props, self.children = name, {}, []

    def child(self, prefix):
        return [c for c in self.children if c.name == prefix or c.name.startswith(prefix + '@')]


def parse(blob):
    magic, totalsize, off_struct, off_strings, off_rsv = struct.unpack_from('>5I', blob, 0)
    if magic != FDT_MAGIC:
        raise SystemExit('not a flattened device tree (bad magic)')
    rsv, p = [], off_rsv
    while True:
        addr, size = struct.unpack_from('>2Q', blob, p)
        p += 16
        if not addr and not size:
            break
        rsv.append((addr, size))

    def cstr(off):
        return blob[off:blob.index(b'\0', off)].decode()

    stack, root, p = [], None, off_struct
    while True:
        tok, = struct.unpack_from('>I', blob, p)
        p += 4
        if tok == BEGIN_NODE:
            name = cstr(p)
            p = (p + len(name) + 1 + 3) & ~3
            n = Node(name)
            if stack:
                stack[-1].children.append(n)
            else:
                root = n
            stack.append(n)
        elif tok == END_NODE:
            stack.pop()
        elif tok == PROP:
            ln, nameoff = struct.unpack_from('>2I', blob, p)
            p += 8
            stack[-1].props[cstr(off_strings + nameoff)] = blob[p:p + ln]
            p = (p + ln + 3) & ~3
        elif tok == NOP:
            continue
        elif tok == END:
            break
        else:
            raise SystemExit(f'bad FDT token {tok:#x}')
    return root, rsv


def cells(node, name, default):
    v = node.props.get(name)
    return struct.unpack('>I', v)[0] if v else default


def regs(node, ac, sc):
    raw = node.props.get('reg', b'')
    words = struct.unpack('>%dI' % (len(raw) // 4), raw)
    out, step = [], ac + sc
    for i in range(0, len(words) - step + 1, step):
        a = 0
        for w in words[i:i + ac]:
            a = a << 32 | w
        s = 0
        for w in words[i + ac:i + step]:
            s = s << 32 | w
        if s:
            out.append((a, s))
    return out


def subtract(free, cut):
    out = []
    cs, ce = cut
    for s, e in free:
        if ce <= s or cs >= e:
            out.append((s, e))
            continue
        if s < cs:
            out.append((s, cs))
        if ce < e:
            out.append((ce, e))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dtb')
    ap.add_argument('--max-size', type=lambda x: int(x, 0), default=0xfcc00000)
    ap.add_argument('--align', type=lambda x: int(x, 0), default=0x2000000)
    ap.add_argument('--all', action='store_true', help='also list windows smaller than 256 MiB')
    a = ap.parse_args()

    root, memreserve = parse(open(a.dtb, 'rb').read())
    ac, sc = cells(root, '#address-cells', 2), cells(root, '#size-cells', 1)

    banks = []
    for n in root.children:
        dt = n.props.get('device_type', b'').rstrip(b'\0')
        if n.name == 'memory' or n.name.startswith('memory@') or dt == b'memory':
            banks += regs(n, ac, sc)
    banks = sorted(set(banks))
    if not banks:
        raise SystemExit('no memory banks with a size: this is not a live FDT '
                         '(ABL fills /memory at boot; pull /sys/firmware/fdt)')

    carve = [(s, z, '/memreserve/') for s, z in memreserve]
    for rm in root.child('reserved-memory'):
        rac, rsc = cells(rm, '#address-cells', ac), cells(rm, '#size-cells', sc)
        for c in rm.children:
            for s, z in regs(c, rac, rsc):
                carve.append((s, z, c.name))
    carve.sort()

    print('memory banks:')
    for s, z in banks:
        print(f'  {s:#012x}-{s + z:#012x}  {z / 2**20:9.1f} MiB')
    print(f'fixed carve-outs ({len(carve)}):')
    for s, z, name in carve:
        print(f'  {s:#012x}-{s + z:#012x}  {z / 2**20:9.2f} MiB  {name}')

    free = [(s, s + z) for s, z in banks]
    for s, z, _ in carve:
        free = subtract(free, (s, s + z))
    wins = []
    for s, e in free:
        base = (s + a.align - 1) & ~(a.align - 1)
        if base < e:
            wins.append((base, e - base))
    wins.sort(key=lambda w: (w[0] >= 1 << 32, w[1]), reverse=True)

    print('free windows (aligned):')
    for b, z in sorted(wins):
        if z >= 256 << 20 or a.all:
            hi = ' (>= 4 GiB)' if b >= 1 << 32 else ''
            print(f'  {b:#012x}-{b + z:#012x}  {z / 2**20:9.1f} MiB{hi}')
    if not wins:
        raise SystemExit('no free window')
    base, size = wins[0]
    size = min(size, a.max_size) & ~(0x4000 - 1)
    print(f'\nsuggested: tools/mkadt.py OUT.adt --board apollo '
          f'--phys-base {base:#x} --mem-size {size:#x}')
    if base < 1 << 32:
        print('warning: no window above 4 GiB; check that ABL does not load the '
              'shim/ramdisk inside this window (the shim refuses an overlapping ramdisk)')


if __name__ == '__main__':
    main()
