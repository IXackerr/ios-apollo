#!/usr/bin/env python3
"""Reassemble a split Qualcomm firmware image (.mdt + .b00 .. .bNN) into one
ELF (.mbn), the form the SWEET GPU driver embeds in the ADT.

  mkzap.py a650_zap.mdt OUT.mbn       # reads a650_zap.b00, .b01 ... next to it
  mkzap.py a650_zap.mbn OUT.mbn       # already one ELF: validated and copied
  mkzap.py --info FILE                # print the program headers

Split layout (what Android vendor partitions ship): the .mdt holds the ELF
header, the program header table and usually the hash segment; segment i
with p_filesz > 0 lives in .bNN (NN = i, two decimal digits) and belongs at
file offset p_offset of the ELF. A segment whose bytes are already inside
the .mdt (the hash segment) has no .bNN file. Same algorithm as Linux
qcom_mdt_load() reading split files, and as the pil-squasher tool.

Only reads; never edits a segment, so the TrustZone signature check (done by
the PAS auth call) still sees exactly the vendor bytes.
"""
import os, struct, sys

PT_LOAD = 1
QCOM_MDT_TYPE_MASK = 7 << 24
QCOM_MDT_TYPE_HASH = 2 << 24


def ehdr(buf):
    if buf[:4] != b'\x7fELF':
        raise SystemExit('not an ELF file')
    cls = buf[4]
    if cls == 1:
        (e_phoff, e_phentsize, e_phnum) = (struct.unpack_from('<I', buf, 0x1c)[0],
                                           struct.unpack_from('<H', buf, 0x2a)[0],
                                           struct.unpack_from('<H', buf, 0x2c)[0])
        fmt = '<IIIIIIII'   # type offset vaddr paddr filesz memsz flags align
    elif cls == 2:
        (e_phoff, e_phentsize, e_phnum) = (struct.unpack_from('<Q', buf, 0x20)[0],
                                           struct.unpack_from('<H', buf, 0x36)[0],
                                           struct.unpack_from('<H', buf, 0x38)[0])
        fmt = '<IIQQQQQQ'   # type flags offset vaddr paddr filesz memsz align
    else:
        raise SystemExit('bad ELF class')
    phdrs = []
    for i in range(e_phnum):
        f = struct.unpack_from(fmt, buf, e_phoff + i * e_phentsize)
        if cls == 1:
            p = dict(type=f[0], offset=f[1], vaddr=f[2], paddr=f[3], filesz=f[4],
                     memsz=f[5], flags=f[6], align=f[7])
        else:
            p = dict(type=f[0], flags=f[1], offset=f[2], vaddr=f[3], paddr=f[4],
                     filesz=f[5], memsz=f[6], align=f[7])
        phdrs.append(p)
    return cls, phdrs


def info(path):
    buf = open(path, 'rb').read()
    cls, phdrs = ehdr(buf)
    print(f'{path}: ELF{32 if cls == 1 else 64}, {len(phdrs)} program headers, {len(buf)} bytes')
    for i, p in enumerate(phdrs):
        kind = 'HASH' if (p['flags'] & QCOM_MDT_TYPE_MASK) == QCOM_MDT_TYPE_HASH else \
            ('LOAD' if p['type'] == PT_LOAD else f'type{p["type"]}')
        print(f'  [{i:2}] {kind:5} off={p["offset"]:#08x} paddr={p["paddr"]:#010x} '
              f'filesz={p["filesz"]:#07x} memsz={p["memsz"]:#07x} flags={p["flags"]:#010x}')


def join(src):
    """The whole image (bytes): .mdt plus its .bNN segment files."""
    mdt = open(src, 'rb').read()
    cls, phdrs = ehdr(mdt)
    end = max([len(mdt)] + [p['offset'] + p['filesz'] for p in phdrs if p['filesz']])
    out = bytearray(end)
    out[:len(mdt)] = mdt
    stem = src[:-4] if src.endswith('.mdt') else None
    for i, p in enumerate(phdrs):
        if not p['filesz']:
            continue
        lo, hi = p['offset'], p['offset'] + p['filesz']
        if hi <= len(mdt):
            continue                 # segment already inside the .mdt (hash)
        if stem is None:
            raise SystemExit(f'{src}: segment {i} beyond end of file (truncated ELF)')
        seg = f'{stem}.b{i:02d}'
        if not os.path.exists(seg):
            raise SystemExit(f'missing {seg} for segment {i}')
        data = open(seg, 'rb').read()
        if len(data) != p['filesz']:
            raise SystemExit(f'{seg}: {len(data)} bytes, program header says {p["filesz"]}')
        out[lo:hi] = data
    return bytes(out)


def squash(src, dst):
    out = join(src)
    open(dst, 'wb').write(out)
    print(f'{dst}: {len(out)} bytes from {src}')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--info':
        info(sys.argv[2])
    elif len(sys.argv) == 3:
        squash(sys.argv[1], sys.argv[2])
        info(sys.argv[2])
    else:
        print(__doc__)
        sys.exit(1)
