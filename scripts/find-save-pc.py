"""Look for the saved script position (pc) inside the save-slot tail.

Idea: when a save is made, the script is normally parked on an input-polling
command (InputGetDown / InputGetState / InputGetEvent ...) waiting for the
player.  So the saved pc should be an instruction address whose command is one
of those.  We collect every such address from the matching .hcb (using the
project's reference decoder) and search the tail for it as a little-endian u32.

Read-only research tool; prints candidates per slot.

Usage:
    python scripts/find-save-pc.py <game_dir> <script.hcb>
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(FVP, 'ref'))

import fvm_ref  # noqa: E402

THUMB = 200 * 113 * 4
# 命令表里这些 id 是"等玩家输入"的轮询命令（ref/cmd_table.json）
INPUT_IDS = {27, 28, 29, 30, 31, 32, 33, 34, 35, 36}


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

    # 指令格式是 `syscall [cmd_id, argc]`：收集"输入轮询命令"的 syscall 地址，
    # 以及紧跟其后的那条指令地址（存档时 pc 通常停在 syscall 之后的等待点）。
    input_addrs = {}
    pcs = sorted(code.instr.keys())
    for idx, pc in enumerate(pcs):
        op, name, args = code.instr[pc]
        if name != 'syscall' or not args:
            continue
        if args[0] in INPUT_IDS:
            input_addrs[pc] = 'syscall %d' % args[0]
            if idx + 1 < len(pcs):
                input_addrs[pcs[idx + 1]] = 'after syscall %d' % args[0]
    print('input-polling instruction addresses: %d' % len(input_addrs))
    if not input_addrs:
        return 1

    sdir = os.path.join(game_dir, 'save')
    names = sorted(n for n in os.listdir(sdir)
                   if n.startswith('s') and n.endswith('.bin') and len(n) == 8
                   and n[1:4].isdigit())
    for name in names:
        tail = tail_of(os.path.join(sdir, name))
        hits = []
        for off in range(0, len(tail) - 4):
            v = struct.unpack_from('<I', tail, off)[0]
            if v in input_addrs:
                hits.append((off, v, input_addrs[v]))
        print('== %s tail=%d' % (name, len(tail)))
        if not hits:
            print('   no input-command address found in the tail')
            continue
        for off, v, cmd in hits[:12]:
            print('   +0x%06x: 0x%06x (%s)' % (off, v, cmd))
        if len(hits) > 12:
            print('   ... %d more' % (len(hits) - 12))
    return 0


if __name__ == '__main__':
    sys.exit(main())