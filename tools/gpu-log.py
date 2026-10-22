#!/usr/bin/env python3
"""Decode the shim GPU probe log from the recovery's pstore console.

shim/gpu.c stores the log three times, COPY bytes apart; a byte is taken
when at least two copies agree, which repairs the bits that flip across
the reset.

    adb shell cat /sys/fs/pstore/console-ramoops-0 > raw
    tools/gpu-log.py raw [OUT]
"""
import sys

COPY = 0x60000


def decode(raw):
    n = len(raw) - 2 * COPY
    if n <= 0:
        raise SystemExit('gpu-log: no shim log in this pstore dump')
    out = bytearray(n)
    for i in range(n):
        a, b, c = raw[i], raw[i + COPY], raw[i + 2 * COPY]
        out[i] = a if a in (b, c) else b if b == c else (a & b) | (a & c) | (b & c)
    return out.decode('ascii', 'replace')


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    text = decode(open(sys.argv[1], 'rb').read())
    if len(sys.argv) == 3:
        open(sys.argv[2], 'w').write(text)
    else:
        sys.stdout.write(text)


if __name__ == '__main__':
    main()
