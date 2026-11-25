"""Diff the variable body of two save slots to find scalar state fields.

The tail map (docs/save-snapshot.md §4.3) fixes these landmarks:

    [0x00000 .. 0x163A0)  32 coroutine contexts
    [0x163A0 .. 0x19408)  always zero in the samples
    [0x19408 .. )         dynamic string pool
    [len-1438 .. len)     constant tail (identical across slots)

Everything between the pool and the constant tail varies with progress.  This
tool walks that region from the pool end forward for two slots and prints the
differing runs, smallest first, so isolated scalar changes (a line number, a
scene counter) stand out from the structural ones.

Read-only research tool.

Usage:
    python scripts/diff-save-body.py <game_dir> <slotA> <slotB> [--limit N]
"""
import os
import struct
import sys

THUMB = 200 * 113 * 4
POOL_OFF = 0x19408
CONST_TAIL = 1438
MAX_RUN = 4096          # 超过这个长度的差异视为"结构性变化"，不再细分


def tail_of(path):
    data = open(path, 'rb').read()
    off = 7
    for _ in range(3):
        n = struct.unpack_from('<H', data, off)[0]
        off += 2 + n
    return data[off + THUMB:]


def pool_end(tail):
    """字符串池的结束位置：u32 tag 0x20 + u32 count + count 条长度前缀字符串。"""
    off = POOL_OFF
    if off + 8 > len(tail):
        return off
    tag, count = struct.unpack_from('<II', tail, off)
    off += 8
    if tag != 0x20:
        return POOL_OFF
    for _ in range(min(count, 128)):
        if off + 2 > len(tail):
            break
        n = struct.unpack_from('<H', tail, off)[0]
        off += 2 + n
    return off


def body(tail):
    """池之后的变长区（到常量尾之前）；池本身长度可变，必须跳过。"""
    end = len(tail) - CONST_TAIL
    start = pool_end(tail)
    if end <= start:
        return b''
    return tail[start:end]


def runs(a, b):
    """Yield (offset, len) of differing runs, forward."""
    n = min(len(a), len(b))
    out = []
    i = 0
    while i < n:
        if a[i] == b[i]:
            i += 1
            continue
        start = i
        while i < n and a[i] != b[i]:
            i += 1
        out.append((start, i - start))
    return out


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    sdir = os.path.join(sys.argv[1], 'save')
    a_path = os.path.join(sdir, sys.argv[2])
    b_path = os.path.join(sdir, sys.argv[3])
    limit = 20
    if '--limit' in sys.argv:
        limit = int(sys.argv[sys.argv.index('--limit') + 1])
    a = body(tail_of(a_path))
    b = body(tail_of(b_path))
    print('body A=%d  body B=%d  (compared %d, pool skipped)'
          % (len(a), len(b), min(len(a), len(b))))
    rs = runs(a, b)
    print('differing runs: %d' % len(rs))
    # smallest runs first: isolated scalar fields are short
    small = sorted([r for r in rs if r[1] <= 64], key=lambda r: r[1])
    print('\n-- isolated runs (<= 64 bytes), smallest first --')
    for off, ln in small[:limit]:
        seg_a = a[off:off + ln]
        seg_b = b[off:off + ln]
        av = struct.unpack_from('<I', a + b'\0\0\0\0', off)[0]
        bv = struct.unpack_from('<I', b + b'\0\0\0\0', off)[0]
        print('  +0x%06x len %3d  A=%s  B=%s   u32 %d -> %d'
              % (off, ln, seg_a.hex(), seg_b.hex(), av, bv))
    print('\n-- largest runs --')
    for off, ln in sorted(rs, key=lambda r: -r[1])[:limit]:
        print('  +0x%06x len %d (%.1f KB)' % (off, ln, ln / 1024.0))
    # first divergence
    if rs:
        off, ln = rs[0]
        print('\nfirst difference at +0x%06x (len %d)' % (off, ln))
    return 0


if __name__ == '__main__':
    sys.exit(main())