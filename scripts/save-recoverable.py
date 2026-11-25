"""What can a save slot tell us today, and what still needs a replay?

`Load(40)` cannot resume execution: the serialized coroutine context has no pc or
stack (docs/save-snapshot.md §4.2).  This tool prints, for every slot, exactly
the fields our parsers recover today, and an explicit list of what a real load
would still need.  It is the machine-checkable form of the §4.6 table.

Usage:
    python scripts/save-recoverable.py <game_dir>
"""
import os
import struct
import sys

THUMB = 200 * 113 * 4
POOL_OFF = 0x19408


def tail_of(path):
    data = open(path, 'rb').read()
    off = 7
    for _ in range(3):
        n = struct.unpack_from('<H', data, off)[0]
        off += 2 + n
    return data[off + THUMB:]


def decode(b):
    for enc in ('cp932', 'gbk', 'utf-8'):
        try:
            s = b.decode(enc)
        except Exception:
            continue
        if not any(0xFF61 <= ord(ch) <= 0xFF9F or ch == '\ufffd' for ch in s):
            return s, enc
    return b.decode('cp932', 'replace'), 'cp932?'


def slot_fields(path):
    data = open(path, 'rb').read()
    year = struct.unpack_from('<H', data, 0)[0]
    month, day, dow, hour, minute = data[2], data[3], data[4], data[5], data[6]
    off = 7
    strs = []
    for _ in range(3):
        n = struct.unpack_from('<H', data, off)[0]
        off += 2
        strs.append(decode(data[off:off + n]))
        off += n
    thumb = len(data) - off - (len(data) - off - THUMB if len(data) - off > THUMB
                               else 0)
    tail = data[off + THUMB:]
    pool = []
    if len(tail) > POOL_OFF + 8:
        tag, count = struct.unpack_from('<II', tail, POOL_OFF)
        if tag == 0x20:
            p = POOL_OFF + 8
            for _ in range(min(count, 8)):
                if p + 2 > len(tail):
                    break
                n = struct.unpack_from('<H', tail, p)[0]
                p += 2
                pool.append(decode(tail[p:p + n]))
                p += n
    ctx_nz = sum(1 for b in tail[:32 * 0xB1D] if b)
    return {
        'time': '%04d-%02d-%02d %02d:%02d (dow=%d)' % (year, month, day, hour,
                                                       minute, dow),
        'str0_chapter': strs[0], 'str1': strs[1], 'str2_line': strs[2],
        'thumb_bytes': THUMB, 'tail_bytes': len(tail),
        'pool': pool, 'ctx_nonzero': ctx_nz,
    }


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    sdir = os.path.join(sys.argv[1], 'save')
    names = sorted(n for n in os.listdir(sdir)
                   if n.startswith('s') and n.endswith('.bin') and len(n) == 8
                   and n[1:4].isdigit())
    for name in names:
        f = slot_fields(os.path.join(sdir, name))
        print('== %s' % name)
        print('   timestamp      : %s' % f['time'])
        print('   chapter (str0) : %r [%s]' % (f['str0_chapter'][0],
                                              f['str0_chapter'][1]))
        print('   line    (str2) : %r [%s]' % (f['str2_line'][0],
                                              f['str2_line'][1]))
        print('   thumbnail      : %d bytes (200x113 BGRA)' % f['thumb_bytes'])
        print('   string pool    : %d entr(ies): %s'
              % (len(f['pool']),
                 ', '.join(repr(s) for s, _e in f['pool']) or '-'))
        print('   tail           : %d bytes, contexts nonzero %d'
              % (f['tail_bytes'], f['ctx_nonzero']))
    print('')
    print('NOT recoverable without a replay (docs/save-snapshot.md §4.6):')
    for item in ('script position (no pc/stack is serialized)',
                 'global variables (not present in the slot tail)',
                 'text window contents beyond the one saved line',
                 'audio state, animations, coroutine stacks'):
        print('  - ' + item)
    return 0


if __name__ == '__main__':
    sys.exit(main())