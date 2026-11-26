"""Analyze the engine-state tail of FVP save slots (read-only research tool).

A slot file is: 7-byte timestamp + 3 length-prefixed strings + 200x113 BGRA
thumbnail + the serialized engine state ("tail").  This tool parses the front
part, then inspects the tail for candidate pointers/positions:

  * u32 values that are valid instruction starts in the matching .hcb
  * u32 values that look like indices into the script's byte range
  * cross-slot comparison at the same offsets (find varying fields)

Usage:
    python scripts/analyze-save-tail.py <game_dir> <script.hcb> [slot files...]
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(FVP, 'ref'))

import fvm_ref  # noqa: E402


def parse_slot(data):
    """Return (stamp, strings, thumb_bytes, tail)."""
    off = 0
    stamp = struct.unpack_from('<HBBBBBB', data, off)
    off += 7
    strings = []
    for _ in range(3):
        n = struct.unpack_from('<H', data, off)[0]
        off += 2
        strings.append(data[off:off + n])
        off += n
    thumb = 200 * 113 * 4
    thumb_bytes = data[off:off + thumb]
    off += thumb
    return stamp, strings, thumb_bytes, data[off:]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    game_dir, script = sys.argv[1], sys.argv[2]
    slots = sys.argv[3:]
    if not slots:
        sdir = os.path.join(game_dir, 'save')
        slots = [os.path.join(sdir, n) for n in sorted(os.listdir(sdir))
                 if n.startswith('s') and n.endswith('.bin') and len(n) == 8]

    code = fvm_ref.Code(open(script, 'rb').read(), 'cp932')
    starts = set(code.instr.keys())

    for path in slots:
        data = open(path, 'rb').read()
        stamp, strings, thumb, tail = parse_slot(data)
        head = tail[:48]
        print('== %s' % os.path.basename(path))
        print('   file %d bytes, tail %d bytes' % (len(data), len(tail)))
        print('   stamp %04u/%02u/%02u %02u:%02u wday=%u' % (
            stamp[0], stamp[1], stamp[2], stamp[4], stamp[5], stamp[3]))
        print('   str0=%r str1=%r str2=%r' % (
            strings[0][:24], strings[1][:24], strings[2][:24]))
        print('   tail head: %s' % head.hex())
        # candidate script positions in the first 4 KiB
        cands = []
        for off in range(0, min(len(tail) - 4, 4096), 4):
            v = struct.unpack_from('<I', tail, off)[0]
            if v in starts:
                cands.append((off, v))
        print('   valid instruction starts in tail[0:4096]: %d' % len(cands))
        for off, v in cands[:12]:
            ins = code.instr[v]
            print('      +0x%04x -> 0x%06x %s%s' % (off, v, ins[1],
                                                    ins[2] if ins[2] else ''))
        # absolute offsets that fall inside the script range
        rng = [struct.unpack_from('<I', tail, o)[0]
               for o in range(0, min(len(tail) - 4, 256), 4)]
        print('   tail[0:256] as u32: %s' % ' '.join('%d' % v for v in rng[:16]))

        # the slot's own "current line" string (str2) sits at a fixed offset in
        # the tail -> the tail is a fixed-layout serialization, not a raw dump.
        for si in (0, 2):
            needle = strings[si]
            if needle:
                pos = tail.find(needle)
                print('   str%d (%d bytes) in tail: %s'
                      % (si, len(needle),
                         ('+0x%x' % pos) if pos >= 0 else 'not found'))
        # structure probes
        last_nz = max((i for i, b in enumerate(tail) if b != 0), default=-1)
        print('   tail nonzero up to +0x%x of +0x%x' % (last_nz, len(tail)))
        if len(tail) > 0x3000:
            a = tail[0x1000:0x2000]
            b = tail[0x2000:0x3000]
            same = sum(1 for x, y in zip(a, b) if x == y)
            print('   4KiB block 0x1000 vs 0x2000 identical: %.1f%%'
                  % (100.0 * same / len(a)))
        # 128 槽动态字符串表：表头在 +0x19408 = u32 线程下标（样本 0x20）+
        # u32 非空条数，随后是 u16 长度前缀的字符串（图名 / 当前对话行）。
        # 条目间的对齐方式还没钉死，所以这里只把原始字节打出来，不做解析
        # （见 docs/save-format.md §2.4.5）。
        if len(tail) > 0x19440:
            print('   tail[0x19400:0x19440]: %s' % tail[0x19400:0x19440].hex())
        print('')


if __name__ == '__main__':
    sys.exit(main())