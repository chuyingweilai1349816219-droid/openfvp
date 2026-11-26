#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""scan-ui-cross.py -- 两部作品（iroseka / hoshimemo）的游戏内 UI 素材名对账。

只做一件事：把 `.hcb` 里 `pushstring` 的 UI 素材名（`sys_` / `menu_` / `fc_` /
`slf_` 四个家族）连同绑定的图元号抄出来，给 `tests/verify_ui_cross.py` 做跨作品
对账。不含几何（几何在 HZC1 头里，见 scripts/scan-ingame-menu.py）与任何图像数据。

用法：
    python scripts/scan-ui-cross.py            # 打印 + 写 ref/ui-cross.json
    python scripts/scan-ui-cross.py --check    # 只比对
"""
import argparse
import hashlib
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
FAMILIES = ('sys_', 'menu_', 'fc_', 'slf_')
OUT = os.path.join(ROOT, 'ref', 'ui-cross.json')


def scan(path):
    data = open(path, 'rb').read()
    code = fvm_ref.Code(data, 'cp932')
    pcs = sorted(code.instr)
    pos = {p: i for i, p in enumerate(pcs)}
    rows = []
    for p in pcs:
        op, name, args = code.instr[p]
        if name != 'pushstring' or not args:
            continue
        asset = args[0]
        if not asset.startswith(FAMILIES):
            continue
        prim = None
        if pos[p] > 0:
            prev = code.instr[pcs[pos[p] - 1]]
            if prev[1] in ('pushint8', 'pushint16'):
                prim = prev[2][0]
        rows.append(dict(asset=asset, family=asset.split('_')[0], prim=prim,
                         load_pc='0x%05x' % p))
    return dict(size=len(data), sha256=hashlib.sha256(data).hexdigest(), loads=rows)


def build():
    out = {}
    for game, path in GAMES.items():
        if not os.path.isfile(path):
            continue
        out[game] = scan(path)
    if not out:
        return None
    families = {}
    for game, g in out.items():
        fam = {}
        for r in g['loads']:
            fam.setdefault(r['family'], []).append(r['asset'])
        families[game] = {k: sorted(v) for k, v in fam.items()}
    return dict(schema='fvp-ui-cross/1',
                note='两作的游戏内 UI 素材名对账（只有名字/图元号/偏移）',
                games=out, families=families)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args(argv)
    payload = build()
    if payload is None:
        print('SKIP: neither hcb found')
        return 0
    for game, g in payload['games'].items():
        fam = {}
        for r in g['loads']:
            fam.setdefault(r['family'], []).append(r['asset'])
        print('%s: %d loads, %s' % (game, len(g['loads']),
                                    {k: len(v) for k, v in sorted(fam.items())}))
    if a.check:
        return 0
    with open(OUT, 'w', encoding='utf-8') as f:
        json.dump(payload, f, ensure_ascii=False, indent=1)
    print('json -> %s' % OUT)
    return 0


if __name__ == '__main__':
    sys.exit(main())