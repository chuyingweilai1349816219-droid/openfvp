"""Landmark-based map of the save-slot tail.

Prior work established these landmarks (docs/save-snapshot.md):

    [0x00000 .. 0x03068)   per-slot globals: G1 = 1549 x 8 bytes
    [0x03068 .. 0x19408)   32 coroutine contexts, 0xB1D bytes each
    [0x19408 .. )          dynamic string table header: u32 thread index (0x20),
                           u32 non-empty count, then count x (u16 length-prefixed)
                           strings followed by zeroed empty slots
    [len-1438 .. len)      constant tail (identical across slots), ends 01 01

This tool prints the map for every slot in a save directory and checks the
invariants, so the "real section order" can be read off the data instead of
being guessed.

Usage:
    python scripts/save-tail-map.py <game_dir>
"""
import os
import struct
import sys

THUMB = 200 * 113 * 4
CTX = 0xB1D
NCTX = 32
CTX_END = NCTX * CTX            # 0x163A0
POOL_OFF = 0x19408
CONST_TAIL = 1438


def tail_of(path):
    data = open(path, 'rb').read()
    off = 7
    for _ in range(3):
        n = struct.unpack_from('<H', data, off)[0]
        off += 2 + n
    return data[off + THUMB:]


def nz(buf):
    return sum(1 for b in buf if b)


def decode(b):
    for enc in ('cp932', 'gbk', 'utf-8'):
        try:
            s = b.decode(enc)
        except Exception:
            continue
        bad = sum(1 for ch in s if 0xFF61 <= ord(ch) <= 0xFF9F or ch == '\ufffd')
        if bad == 0:
            return s, enc
    return b.decode('cp932', 'replace'), 'cp932?'


def pool_strings(tail):
    out = []
    off = POOL_OFF
    if off + 8 > len(tail):
        return out
    tag, count = struct.unpack_from('<II', tail, off)
    if tag != 0x20:
        return [('tag=0x%x (unexpected)' % tag, '')]
    off += 8
    for _ in range(min(count, 8)):
        if off + 2 > len(tail):
            break
        n = struct.unpack_from('<H', tail, off)[0]
        off += 2
        raw = tail[off:off + n]
        off += n
        out.append(decode(raw))
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    sdir = os.path.join(sys.argv[1], 'save')
    names = sorted(n for n in os.listdir(sdir)
                   if n.startswith('s') and n.endswith('.bin') and len(n) == 8
                   and n[1:4].isdigit())
    hashes = {}
    for name in names:
        tail = tail_of(os.path.join(sdir, name))
        n = len(tail)
        ctx_nz = nz(tail[:CTX_END])
        gap = tail[CTX_END:POOL_OFF] if n > POOL_OFF else b''
        ct = tail[n - CONST_TAIL:]
        import hashlib
        h = hashlib.sha256(ct).hexdigest()[:16]
        hashes[name] = h
        print('== %s  tail=%d' % (name, n))
        print('   contexts 0x00000..0x%05X : %d nonzero byte(s)' % (CTX_END,
                                                                   ctx_nz))
        print('   gap      0x%05X..0x%05X : %d nonzero byte(s)'
              % (CTX_END, POOL_OFF, nz(gap)))
        if n > POOL_OFF:
            strings = pool_strings(tail)
            print('   string pool @0x%05X : %d string(s)' % (POOL_OFF,
                                                             len(strings)))
            for s, enc in strings:
                print('       [%s] %r' % (enc, s))
        print('   body 0x%05X..0x%05X : %d byte(s) (%d nonzero)'
              % (POOL_OFF, n - CONST_TAIL, max(0, n - CONST_TAIL - POOL_OFF),
                 nz(tail[POOL_OFF:n - CONST_TAIL])))
        print('   constant tail (last %d B): sha256=%s  last2=%s'
              % (CONST_TAIL, h, ct[-2:].hex()))
    print('')
    uniq = sorted(set(hashes.values()))
    print('constant-tail hashes: %d distinct -> %s'
          % (len(uniq), 'all slots identical' if len(uniq) == 1 else
             'MISMATCH across slots'))
    return 0


if __name__ == '__main__':
    sys.exit(main())