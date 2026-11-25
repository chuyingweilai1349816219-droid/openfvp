"""Map the 32 coroutine contexts at the start of a save tail.

The slot serializer writes 32 contexts of 0xB1D bytes each right after the
thumbnail (docs/save-snapshot.md §4.2).  This tool prints, per context:

  * how many non-zero bytes the record holds (idle contexts are nearly empty),
  * the 7 u32 fields and the u8 field, decoded from their stream offsets,

so the layout can be checked against the data without guessing.

Usage:
    python scripts/save-context-map.py <game_dir> [slot]
"""
import os
import struct
import sys

THUMB = 200 * 113 * 4
CTX = 0xB1D
# 流内偏移 -> 字段名（来自 0x445640 的写出顺序）
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


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    sdir = os.path.join(sys.argv[1], 'save')
    want = sys.argv[2] if len(sys.argv) > 2 else None
    names = sorted(n for n in os.listdir(sdir)
                   if n.startswith('s') and n.endswith('.bin') and len(n) == 8
                   and n[1:4].isdigit())
    for name in names:
        if want is not None and name != 's%s.bin' % want:
            continue
        tail = tail_of(os.path.join(sdir, name))
        print('== %s tail=%d (32 contexts = %d B)' % (name, len(tail), 32 * CTX))
        total = 0
        for i in range(32):
            base = i * CTX
            if base + CTX > len(tail):
                break
            blk = tail[base:base + CTX]
            nz = sum(1 for b in blk if b)
            total += nz
            if nz == 0:
                continue
            u8 = blk[0x810]
            vals = [struct.unpack_from('<I', blk, off)[0] for off, _ in FIELDS]
            print('  ctx %2d  nonzero %4d  u8=0x%02x  %s'
                  % (i, nz, u8,
                     ' '.join('%s=0x%x' % (nm.split('+')[1], v)
                              for (_, nm), v in zip(FIELDS, vals))))
        print('  total nonzero in the context area: %d / %d bytes'
              % (total, 32 * CTX))
    return 0


if __name__ == '__main__':
    sys.exit(main())