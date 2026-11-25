"""Diff the serialized coroutine contexts of two saves from the same scene.

Layout per context (from 0x445640/0x445540, see docs/save-snapshot.md §4.2):

    +0x000  0x800 bytes  ctx+0x008 (text buffer)
    +0x800  u32          ctx+0x814
    +0x804  u32          ctx+0x818
    +0x808  u32          ctx+0x808
    +0x80C  u32          ctx+0x80C
    +0x810  u8           ctx+0x810
    +0x811  u32          ctx+0x81C
    +0x815  u32          ctx+0x820
    +0x819  u32          ctx+0x824
    +0x81D  0x100 bytes
    +0x91D  0x200 bytes
    total   0xB1D

Usage:
    python scripts/diff-save-contexts.py <game_dir> <slotA.bin> <slotB.bin>
"""
import os
import struct
import sys

THUMB = 200 * 113 * 4
CTX = 0xB1D
FIELDS = [(0x800, 'ctx+0x814'), (0x804, 'ctx+0x818'), (0x808, 'ctx+0x808'),
          (0x80C, 'ctx+0x80C'), (0x811, 'ctx+0x81C'), (0x815, 'ctx+0x820'),
          (0x819, 'ctx+0x824')]


def tail_of(path):
    data = open(path, 'rb').read()
    off = 7
    for _ in range(3):
        n = struct.unpack_from('<H', data, off)[0]
        off += 2 + n
    return data[off + THUMB:]


def ctx_fields(tail, i):
    base = i * CTX
    out = {}
    if base + CTX > len(tail):
        return None
    for off, name in FIELDS:
        out[name] = struct.unpack_from('<I', tail, base + off)[0]
    out['u8 ctx+0x810'] = tail[base + 0x810]
    return out


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    a = tail_of(sys.argv[2])
    b = tail_of(sys.argv[3])
    print('A tail=%d   B tail=%d' % (len(a), len(b)))
    for i in range(32):
        fa = ctx_fields(a, i)
        fb = ctx_fields(b, i)
        if fa is None or fb is None:
            break
        diffs = [(k, fa[k], fb[k]) for k in fa if fa[k] != fb[k]]
        nz = sum(1 for k in fa if fa[k] or fb[k])
        if not diffs and nz == 0:
            continue
        print('ctx %2d : %d field(s) differ (non-zero fields: %d)'
              % (i, len(diffs), nz))
        for k, va, vb in diffs:
            print('    %-12s A=0x%08x B=0x%08x   (int %d -> %d, delta %+d)'
                  % (k, va, vb, va, vb, vb - va))
    return 0


if __name__ == '__main__':
    sys.exit(main())