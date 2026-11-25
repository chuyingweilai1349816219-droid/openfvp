"""Test the documented layout: the save tail starts with 32 coroutine contexts
of 0xB28 bytes each (docs/save-format.md §2.4.4 item 4).

For every context block we look for u32 values that are valid instruction
addresses of the matching .hcb.  If the *same* (context index, offset inside the
block) appears in several slots, that field is very likely the saved pc.

Read-only research tool.

Usage:
    python scripts/find-save-contexts2.py <game_dir> <script.hcb>
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(FVP, 'ref'))

import fvm_ref  # noqa: E402

THUMB = 200 * 113 * 4
CTX = 0xB28   # 每个协程上下文 2856 字节
NCTX = 32


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
    field_hits = {}
    for name in names:
        tail = tail_of(os.path.join(sdir, name))
        hits = []
        for ctx in range(NCTX):
            base = ctx * CTX
            if base + CTX > len(tail):
                break
            for off in range(0, CTX - 4, 4):
                v = struct.unpack_from('<I', tail, base + off)[0]
                if v in starts:
                    hits.append((ctx, off, v))
                    field_hits.setdefault((ctx, off), []).append((name, v))
        print('== %s: %d instruction-address u32 inside the 32 context blocks'
              % (name, len(hits)))
        for ctx, off, v in hits[:8]:
            print('   ctx %2d +0x%03x -> 0x%06x' % (ctx, off, v))
        if len(hits) > 8:
            print('   ... %d more' % (len(hits) - 8))

    print('\n== fields seen in >=2 slots (candidate saved pc) ==')
    for (ctx, off), vals in sorted(field_hits.items(),
                                   key=lambda kv: -len(kv[1])):
        if len(vals) < 2:
            continue
        print('   ctx %2d +0x%03x : %s'
              % (ctx, off, ', '.join('%s=0x%06x' % v for v in vals[:6])))
    return 0


if __name__ == '__main__':
    sys.exit(main())