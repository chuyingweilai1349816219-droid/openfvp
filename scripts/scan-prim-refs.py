#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""scan-prim-refs.py -- 在 HCB 字节码里找某个图元号被哪些指令引用。

动机：标题菜单那段 `0x592DF` 的等待循环在等 `MotionAlphaTest(265)` 的动作记录，
而 265 的初始化/动画代码散落在十几个位置。要判断"哪条路径没跑到"，先得有一份
"这个图元号到底在哪被引用"的清单——这个脚本就是干这件事的（纯静态扫描，不跑游戏）。

做法：HCB 的图元号是 `pushint16` 的立即数（opcode 0x0b + u16 小端），也可能是
`pushint8`（0x0c + u8，id ≤ 255）。脚本扫这两种编码，命中后用 ref/fvm_ref.py 的
解码器把上下文 6 条指令解出来打印；`--json` 输出机器可读结果，供门禁比对。

用法：
    python scripts/scan-prim-refs.py 265
    python scripts/scan-prim-refs.py 265 --json out/prim-265.json
    python scripts/scan-prim-refs.py 265 --game hoshimemo
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

OP_PUSH16 = 0x0b
OP_PUSH8 = 0x0c


def decode_at(code, pc, count=6):
    """从 pc 起顺序解 count 条指令，返回 [(pc, name, args), ...]。"""
    out = []
    for _ in range(count):
        ins = code.at(pc)
        if ins is None:
            break
        op, name, args = ins
        out.append((pc, name, args))
        pc += code.size[pc]
    return out


def scan(path, prim):
    """返回 [(offset, kind, [(pc, name, args)...]), ...]。"""
    data = open(path, 'rb').read()
    code = fvm_ref.Code(data, 'cp932')
    hits = []
    if prim <= 0xFF:
        pat8 = bytes([OP_PUSH8, prim & 0xff])
        i = 0
        while True:
            i = data.find(pat8, i)
            if i < 0:
                break
            hits.append((i, 'pushint8'))
            i += 1
    pat16 = bytes([OP_PUSH16, prim & 0xff, (prim >> 8) & 0xff])
    i = 0
    while True:
        i = data.find(pat16, i)
        if i < 0:
            break
        hits.append((i, 'pushint16'))
        i += 1
    hits.sort()
    out = []
    for off, kind in hits:
        out.append((off, kind, decode_at(code, off)))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description='scan HCB for prim-id references')
    ap.add_argument('prim', type=lambda s: int(s, 0))
    ap.add_argument('--game', default='iroseka', choices=sorted(GAMES))
    ap.add_argument('--json', default='', help='write machine-readable JSON here')
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args(argv)

    path = GAMES[a.game]
    if not os.path.isfile(path):
        print('SKIP: %s not found (%s)' % (a.game, path))
        return 0
    refs = scan(path, a.prim)
    if not a.quiet:
        print('%s: prim %d referenced %d time(s)' % (a.game, a.prim, len(refs)))
        for off, kind, seq in refs:
            body = ' | '.join('%s%s' % (n, arg if arg else '') for _, n, arg in seq)
            print('  %05x %-9s %s' % (off, kind, body))
    if a.json:
        payload = dict(game=a.game, prim=a.prim, count=len(refs), sites=[
            dict(offset='0x%05x' % off, kind=kind,
                 instr=[dict(pc='0x%05x' % pc, name=name, args=args)
                        for pc, name, args in seq])
            for off, kind, seq in refs])
        with open(a.json, 'w', encoding='utf-8') as fp:
            json.dump(payload, fp, ensure_ascii=False, indent=1)
        if not a.quiet:
            print('json -> %s' % a.json)
    return 0


if __name__ == '__main__':
    sys.exit(main())