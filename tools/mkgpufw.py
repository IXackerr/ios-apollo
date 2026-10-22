#!/usr/bin/env python3
"""Pack the Adreno firmware into the container the shim GPU probe reads
from its ramdisk (shim/gpu.c sgpu_dt_prop): the same properties mkadt.py
puts into the /sweet-gpu ADT node.

    tools/mkgpufw.py FWDIR OUT
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkadt  # noqa: E402


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    fwdir, out = sys.argv[1:]
    gpu = mkadt.gpu_node(fwdir, lambda: ('AAPL,phandle', b''))
    props = [(n, v) for n, _, v in gpu.props if n not in ('AAPL,phandle', 'compatible', 'name')]
    off = 8 + 32 * len(props)
    head, body = [], b''
    for name, value in props:
        pad = (-(off + len(body))) % 16
        body += b'\0' * pad
        head.append(struct.pack('<24sII', name.encode(), off + len(body), len(value)))
        body += value
    with open(out, 'wb') as f:
        f.write(struct.pack('<4sI', b'SGFW', len(props)) + b''.join(head) + body)
    print(f'mkgpufw: {", ".join(n for n, _ in props)} -> {out}', file=sys.stderr)


if __name__ == '__main__':
    main()
