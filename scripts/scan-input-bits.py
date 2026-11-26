#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""scan-input-bits.py -- 两部作品的脚本到底测了 InputGetDown/InputGetState 的哪些位。

判据：`syscall 31`（InputGetDown）/`syscall 34`（InputGetState）之后 5 条指令内的
`pushint8 N`，N 就是脚本拿去 `test` 的**掩码值**（不是位号）。本脚本按掩码值计数，
并换算成"第 n 位被用到多少次"，产物 `ref/input-bits.json`。

为什么不用"bit1"这种说法：掩码值 1 是**第 0 位**，掩码值 2 才是第 1 位；历史上两种
写法混用过，所以这里一律记成 `mask=1 (bit0)`。

用法：
    python scripts/scan-input-bits.py            # 打印 + 写 json
    python scripts/scan-input-bits.py --check
"""
import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'ref'))
import fvm_ref  # noqa: E402

GAMES = {
    'iroseka': os.path.join(r'C:\Users\Mechrevo\Downloads',
                            '\u4e94\u5f69\u6591\u6593\u7684\u4e16\u754cHD',
                            'iroseka.hcb'),
    'hoshimemo': os.path.join(r'C:\Users\Mechrevo\Downloads',
                              'hoshimemo_installed', 'Hoshimemo_HD.hcb'),
}
INPUT_SYSCALLS = {31: 'InputGetDown', 34: 'InputGetState'}
OUT = os.path.join(ROOT, 'ref', 'input-bits.json')


def scan(path):
    data = open(path, 'rb').read()
    code = fvm_ref.Code(data, 'cp932')
    pcs = sorted(code.instr)
    pos = {p: i for i, p in enumerate(pcs)}
    masks = {}
    per_cmd = {}
    for p in pcs:
        op, name, args = code.instr[p]
        if name != 'syscall' or not args or args[0] not in INPUT_SYSCALLS:
            continue
        cmd = INPUT_SYSCALLS[args[0]]
        per_cmd[cmd] = per_cmd.get(cmd, 0) + 1
        mask = None
        for d in range(1, 6):
            if pos[p] + d >= len(pcs):
                break
            q = code.instr[pcs[pos[p] + d]]
            if q[1] == 'pushint8':
                mask = q[2][0]
                break
        key = str(mask) if mask is not None else 'none'
        masks[key] = masks.get(key, 0) + 1
    bits = {}
    for k, n in masks.items():
        if k == 'none':
            continue
        v = int(k)
        for b in range(8):
            if v & (1 << b):
                bits[str(b)] = bits.get(str(b), 0) + n
    return dict(masks=masks, bits=bits, calls=per_cmd)


def build():
    out = {}
    for game, path in GAMES.items():
        if not os.path.isfile(path):
            continue
        out[game] = scan(path)
    if not out:
        return None
    return dict(schema='fvp-input-bits/1',
                note=('脚本侧实际使用的输入掩码值（不是位号）：mask=1 是第 0 位。'
                      '计数器来自 syscall 31/34 之后 5 条指令内的 pushint8。'),
                games=out)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args(argv)
    payload = build()
    if payload is None:
        print('SKIP: neither hcb found')
        return 0
    for game, g in payload['games'].items():
        masks = sorted(((int(k), v) for k, v in g['masks'].items() if k != 'none'))
        print('%s: %s, masks %s, bits %s'
              % (game, g['calls'], masks, sorted(g['bits'].items(), key=lambda kv: int(kv[0]))))
    if a.check:
        return 0
    with open(OUT, 'w', encoding='utf-8') as f:
        json.dump(payload, f, ensure_ascii=False, indent=1)
    print('json -> %s' % OUT)
    return 0


if __name__ == '__main__':
    sys.exit(main())