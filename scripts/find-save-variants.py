"""Locate the variant-array (globals) region in the save tail.

Observation from the raw bytes: the tail contains 8-byte records that look like
{u32 value, u32 type} — e.g. `0e ab 05 00 | 02 00 00 00` (int 371470).  If that
is the global-variable table, there should be a long run of such records with a
small type field.  This tool measures the longest such run per slot.

Read-only research tool.

Usage:
    python scripts/find-save-variants.py <game_dir>
"""
import os
import struct
import sys

THUMB = 200 * 113 * 4
MAX_TYPE = 8  # 观测到的类型值都很小；超过就认为不是 Variant 记录


def tail_of(path):
    data = open(path, 'rb').read()
    off = 7
    for _ in range(3):
        n = struct.unpack_from('<H', data, off)[0]
        off += 2 + n
    return data[off + THUMB:]


def is_record(tail, off, skip_unset=False):
    if off + 8 > len(tail):
        return False
    v, t = struct.unpack_from('<II', tail, off)
    if t > MAX_TYPE:
        return False
    if skip_unset and t == 0:
        return False
    # 值不能离谱地大（脚本地址 < 8 MB；整数/浮点都可以，但排除明显的垃圾）
    if v > 0x10000000 and t != 2:
        return False
    return True


def longest_run(tail, skip_unset=False):
    best = (0, 0)
    off = 0
    n = len(tail)
    while off + 8 <= n:
        if not is_record(tail, off, skip_unset):
            off += 4
            continue
        start = off
        run = 0
        while off + 8 <= n and is_record(tail, off, skip_unset):
            run += 1
            off += 8
            if run > 20000:
                break
        if run > best[0]:
            best = (run, start)
    return best


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    skip = '--skip-unset' in sys.argv
    sdir = os.path.join(sys.argv[1], 'save')
    names = sorted(n for n in os.listdir(sdir)
                   if n.startswith('s') and n.endswith('.bin') and len(n) == 8
                   and n[1:4].isdigit())
    for name in names:
        tail = tail_of(os.path.join(sdir, name))
        run, start = longest_run(tail, skip)
        print('== %s tail=%d' % (name, len(tail)))
        if run < 32:
            print('   no long {u32 value, u32 type} run (best %d)' % run)
            continue
        print('   longest run: %d records @+0x%06x (%.1f KB)'
              % (run, start, run * 8 / 1024.0))
        hist = {}
        for k in range(min(run, 4096)):
            t = struct.unpack_from('<II', tail, start + 8 * k)[1]
            hist[t] = hist.get(t, 0) + 1
        print('   type histogram: %s' % ', '.join(
            '%d:%d' % (k, v) for k, v in sorted(hist.items()))[:200])
    return 0


if __name__ == '__main__':
    sys.exit(main())