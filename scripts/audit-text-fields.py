"""Audit which text-window fields are actually READ by the original renderer.

The static listing (`work/iroseka_HD.asm`, produced with objdump) shows every access
to a struct displacement.  For the text-window fields that commands 118
(TextFormat) / 125 (TextShadowDist) / 128 (TextSpace) write, we want to know
whether the renderer ever reads them back — if it does not, then "record the
value and don't render it" is what the original does too, not a shortcut.

Usage:
    python scripts/audit-text-fields.py [--asm work/iroseka_HD.asm]

Read-only; the .asm is a local artefact (work/ is git-ignored).
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)

# 文本窗口结构里被 118/125/128 写过的字段（见 docs/vm-commands.md 与
# docs/render-model.md §7.1）：
FIELDS = [
    (0x5A8, 'TextSpace x / TextFormat a'),
    (0x5AA, 'TextSpace y / TextFormat b'),
    (0x5AC, 'TextFormat d'),
    (0x5AE, 'TextFormat c'),
    (0x5B0, 'TextFormat f'),
    (0x5B4, 'TextFormat e'),
    (0x596, 'shadow 前置标志（0x4248B0 附近）'),
    (0x597, 'TextShadowDist'),
    (0x5E8, 'd 的去向（文本窗口字段）'),
]


def classify(line, off):
    """Return 'read' / 'write' / None for one static listing line."""
    disp = '0x%x(' % off
    if disp not in line:
        return None
    text = line.split('\t')[-1].strip()
    if text.startswith('mov') or text.startswith('movz') or \
            text.startswith('movs'):
        # mov  %reg,0x5a8(%esi)   -> write
        # mov  0x5ac(%esi),%ax    -> read
        m = re.match(r'^\S+\s+(.+)$', text)
        if not m:
            return None
        ops = [x.strip() for x in m.group(1).split(',')]
        if len(ops) < 2:
            return None
        # AT&T 语法：mov %src,%dst —— 内存出现在**最后一个**操作数就是写入，
        # 出现在第一个操作数才是读取。
        if disp in ops[-1]:
            return 'write'
        if disp in ops[0]:
            return 'read'
        return 'other'
    if text.startswith(('add', 'or', 'and', 'inc', 'dec')):
        return 'read-write'
    return 'other'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--asm', default=os.path.join(FVP, 'work',
                                                  'iroseka_HD.asm'))
    ap.add_argument('--context', type=int, default=0,
                    help='读点前后各打印 N 行静态分析（分析语义用）')
    args = ap.parse_args()
    if not os.path.isfile(args.asm):
        print('audit-text-fields: missing %s (analyse statically iroseka_HD.exe first)'
              % args.asm, file=sys.stderr)
        return 2

    hits = {off: {'read': [], 'write': [], 'read-write': [], 'other': []}
            for off, _ in FIELDS}
    with open(args.asm, encoding='utf-8', errors='replace') as f:
        for line in f:
            for off, _name in FIELDS:
                kind = classify(line, off)
                if kind is not None:
                    hits[off][kind].append(line.strip())

    print('%-8s %-38s %5s %6s' % ('field', 'meaning', 'read', 'write'))
    for off, name in FIELDS:
        h = hits[off]
        print('0x%04x  %-38s %5d %6d'
              % (off, name, len(h['read']) + len(h['read-write']),
                 len(h['write']) + len(h['read-write'])))
    print('')
    n_ctx = args.context
    all_lines = []
    if n_ctx > 0:
        with open(args.asm, encoding='utf-8', errors='replace') as f:
            all_lines = f.read().split('\n')
    for off, name in FIELDS:
        h = hits[off]
        reads = h['read'] + h['read-write']
        if not reads:
            continue
        print('-- 0x%04x (%s) 的读点：' % (off, name))
        for line in reads:
            print('   ' + line[:100])
            if n_ctx > 0:
                addr = line.split(':')[0].strip()
                idx = next((k for k, l in enumerate(all_lines)
                            if l.strip().startswith(addr + ':')), None)
                if idx is not None:
                    for l in all_lines[max(0, idx - n_ctx):idx + n_ctx + 1]:
                        print('       ' + l.rstrip()[:96])
                    print('       ---')
    return 0


if __name__ == '__main__':
    sys.exit(main())