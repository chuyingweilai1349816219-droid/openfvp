#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""scan-ui-modules.py -- 两部作品的 UI 模块地址对账（环形菜单装载器 / 确认框装载器）。

上一轮证明两作的素材名与图元号一致；这一轮把**脚本侧地址**也对起来，回答
"两作的同一模块在各自 .hcb 里的偏移是多少"。判据全部是结构性的：

1. 环形菜单装载器 = 第一个 `sys_*` pushstring 所在的函数；
2. 确认框装载器 = 第一个 `menu_*`（不含 `menu_byebye`）pushstring 所在的函数；
3. 每个装载器**只有一个调用点**；调用点所在函数即该模块的"入口函数"。

产物 `ref/ui-modules.json` 只含地址、指令数、素材数与图元号范围。

用法：
    python scripts/scan-ui-modules.py            # 打印 + 写 json
    python scripts/scan-ui-modules.py --check    # 只比对
"""
import argparse
import bisect
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
OUT = os.path.join(ROOT, 'ref', 'ui-modules.json')


def scan(path):
    data = open(path, 'rb').read()
    code = fvm_ref.Code(data, 'cp932')
    pcs = sorted(code.instr)
    starts = [p for p in pcs if code.instr[p][1] == 'initstack']

    def fn_of(pc):
        k = bisect.bisect_right(starts, pc) - 1
        return starts[k] if k >= 0 else None

    def first_asset(prefix, exclude=()):
        for p in pcs:
            op, name, args = code.instr[p]
            if name != 'pushstring' or not args:
                continue
            a = args[0]
            if a.startswith(prefix) and not any(a.startswith(x) for x in exclude):
                return p, a
        return None, None

    def describe(loader, prefix, exclude=()):
        inside = [p for p in pcs if fn_of(p) == loader]
        assets = [code.instr[p][2][0] for p in inside
                  if code.instr[p][1] == 'pushstring' and code.instr[p][2]
                  and code.instr[p][2][0].startswith(prefix)
                  and not any(code.instr[p][2][0].startswith(x) for x in exclude)]
        prims = []
        for p in inside:
            op, name, args = code.instr[p]
            if name == 'pushstring' and args and args[0].startswith(prefix) and inside:
                i = pcs.index(p) if p in pcs else -1
                if i > 0:
                    prev = code.instr[pcs[i - 1]]
                    if prev[1] in ('pushint8', 'pushint16'):
                        prims.append(prev[2][0])
        callers = ['0x%05x' % q for q in pcs
                   if code.instr[q][1] == 'call' and code.instr[q][2]
                   and code.instr[q][2][0] == loader]
        return dict(fn='0x%05x' % loader, instructions=len(inside),
                    assets=len(assets), prim_min=min(prims) if prims else None,
                    prim_max=max(prims) if prims else None,
                    callers=callers,
                    entry=('0x%05x' % fn_of(int(callers[0], 16)))
                    if len(callers) == 1 else None)

    ring_pc, ring_asset = first_asset('sys_')
    dlg_pc, dlg_asset = first_asset('menu_', exclude=('menu_byebye',))
    out = dict(size=len(data), hcb_sha256=_sha(data))
    if ring_pc is not None:
        out['ring'] = describe(fn_of(ring_pc), 'sys_')
        out['ring']['first_asset'] = ring_asset
        out['ring']['first_pc'] = '0x%05x' % ring_pc
    if dlg_pc is not None:
        out['dialog'] = describe(fn_of(dlg_pc), 'menu_')
        out['dialog']['first_asset'] = dlg_asset
        out['dialog']['first_pc'] = '0x%05x' % dlg_pc
    return out


def _sha(data):
    import hashlib
    return hashlib.sha256(data).hexdigest()


def build():
    out = {}
    for game, path in GAMES.items():
        if not os.path.isfile(path):
            continue
        out[game] = scan(path)
    if not out:
        return None
    return dict(schema='fvp-ui-modules/1',
                note='两作 UI 模块的脚本侧地址对账（环形菜单装载器 / 确认框装载器）',
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
        for key in ('ring', 'dialog'):
            m = g.get(key)
            if not m:
                continue
            print('%s %-6s fn=%s instrs=%d assets=%d prims=%s..%s entry=%s callers=%d'
                  % (game, key, m['fn'], m['instructions'], m['assets'],
                     m['prim_min'], m['prim_max'], m['entry'], len(m['callers'])))
    if a.check:
        return 0
    with open(OUT, 'w', encoding='utf-8') as f:
        json.dump(payload, f, ensure_ascii=False, indent=1)
    print('json -> %s' % OUT)
    return 0


if __name__ == '__main__':
    sys.exit(main())