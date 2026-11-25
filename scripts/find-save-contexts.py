"""Find runs of consecutive valid instruction addresses in the save tail.

A saved script often stores a *context array* (one pc per thread).  If several
neighbouring u32s are all valid instruction starts of the matching .hcb, that
region is very likely the saved context table.  Read-only research tool.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(FVP, 'ref'))

import fvm_ref  # noqa: E402

THUMB = 200 * 113 * 4


def tail_of(path):
    data = open(path, 'rb').read()
    off = 7
    for _ in range(3):
        n = struct.unpack_from('<H', data, off)[0]
        off += 2 + n
    return data[off + THUMB:]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    game_dir, script = sys.argv[1], sys.argv[2]
    with open(script, 'rb') as f:
        code = fvm_ref.Code(f.read(), 'cp932')
    starts = set(code.instr.keys())

    sdir = os.path.join(game_dir, 'save')
    names = sorted(n for n in os.listdir(sdir)
                   if n.startswith('s') and n.endswith('.bin') and len(n) == 8
                   and n[1:4].isdigit())
    for name in names:
        tail = tail_of(os.path.join(sdir, name))
        best = []
        n = len(tail)
        for base in range(0, n - 4 * 4, 4):
            run = 0
            off = base
            while off + 4 <= n:
                v = struct.unpack_from('<I', tail, off)[0]
                if v not in starts:
                    break
                run += 1
                off += 4
                if run > 64:
                    break
            if run >= 8:
                best.append((run, base))
        best.sort(reverse=True)
        print('== %s tail=%d' % (name, len(tail)))
        if not best:
            print('   no run of >= 8 valid instruction addresses')
            continue
        for run, base in best[:6]:
            vals = [struct.unpack_from('<I', tail, base + 4 * k)[0]
                    for k in range(min(run, 6))]
            print('   run %2d @+0x%06x  %s' % (run, base,
                                               ' '.join('0x%06x' % v for v in vals)))
    return 0


if __name__ == '__main__':
    sys.exit(main())