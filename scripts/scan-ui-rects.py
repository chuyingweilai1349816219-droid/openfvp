#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""scan-ui-rects.py -- 直接从 `.bin` 归档里读 UI 素材的 HZC1 头，取屏幕矩形。

为什么不用 `extract/graph/`：那是 `bin_archiver` 解包出来的中间产物，只对五彩做过；
星空的作品还没解包（677 MB）。`FVP` 的包格式很简单（`docs/hzc1-format.md` §2）：

    u32 entry_count, u32 name_table_len
    entry_count × (u32 name_off, u32 data_off, u32 size)
    name_table[name_table_len]  （CP932，NUL 结尾）

所以这里直接按名字定位、只读每个素材的 44 字节头（`0x14/0x16/0x18/0x1A` =
w/h/x/y），不需要解包、也不落盘任何像素。两个游戏都用这一条路径，几何可以互相对账
（五彩还能和 `ref/ingame-menu.json` 里"解包文件"那条路径交叉验证）。

用法：
    python scripts/scan-ui-rects.py --game iroseka
    python scripts/scan-ui-rects.py --game hoshimemo --json out/x.json
"""
import argparse
import hashlib
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CROSS = os.path.join(ROOT, 'ref', 'ui-cross.json')
OUT = os.path.join(ROOT, 'ref', 'ui-rects.json')

GAMES = {
    'iroseka': os.path.join(r'C:\Users\Mechrevo\Downloads',
                            '\u4e94\u5f69\u6591\u6593\u7684\u4e16\u754cHD'),
    'hoshimemo': os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed'),
}
BINS = ['graph', 'graph_bg', 'graph_bs', 'graph_vis', 'graph_vis1', 'graph_vis2']


def index_bin(path):
    """返回 {name: (data_off, size)}。"""
    with open(path, 'rb') as f:
        head = f.read(8)
        if len(head) < 8:
            return {}
        n, namelen = struct.unpack('<II', head)
        raw = f.read(n * 12)
        names = f.read(namelen)
    out = {}
    for i in range(n):
        noff, doff, size = struct.unpack_from('<III', raw, i * 12)
        end = names.find(b'\x00', noff)
        if end < 0:
            end = len(names)
        nm = names[noff:end].decode('cp932', 'replace')
        out[nm] = (doff, size)
    return out


def read_rect(path, off):
    with open(path, 'rb') as f:
        f.seek(off)
        h = f.read(0x2C)
    if len(h) < 0x2C or h[:4] != b'hzc1' or h[12:16] != b'NVSG':
        return None
    w = struct.unpack_from('<H', h, 0x14)[0]
    ht = struct.unpack_from('<H', h, 0x16)[0]
    x = struct.unpack_from('<H', h, 0x18)[0]
    y = struct.unpack_from('<H', h, 0x1A)[0]
    return [x, y, w, ht]


def game_assets(game):
    with open(CROSS, encoding='utf-8') as f:
        cross = json.load(f)
    fam = cross['families'][game]
    names = []
    for fam_name in sorted(fam):
        names.extend(sorted(fam[fam_name]))
    return names


def scan(game):
    gdir = GAMES[game]
    idx = {}
    for b in BINS:
        p = os.path.join(gdir, b + '.bin')
        if not os.path.isfile(p):
            continue
        for nm, ent in index_bin(p).items():
            idx.setdefault(nm, (p,) + ent)
    rects, missing = {}, []
    for name in game_assets(game):
        ent = idx.get(name)
        if ent is None:
            missing.append(name)
            continue
        rect = read_rect(ent[0], ent[1])
        if rect is None:
            missing.append(name)
            continue
        rects[name] = rect
    return dict(rects=rects, missing=missing, bins=len(idx))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default='all', choices=['all'] + sorted(GAMES))
    ap.add_argument('--json', default='')
    a = ap.parse_args(argv)
    games = sorted(GAMES) if a.game == 'all' else [a.game]
    out = {}
    for game in games:
        gdir = GAMES[game]
        if not os.path.isdir(gdir):
            print('SKIP: %s not found' % gdir)
            continue
        got = scan(game)
        out[game] = got
        print('%s: %d assets with header rect, %d missing'
              % (game, len(got['rects']), len(got['missing'])))
        if got['missing']:
            print('  missing: %s' % ', '.join(got['missing'][:4]) +
                  (' …' if len(got['missing']) > 4 else ''))
    if not out:
        return 0
    if a.json:
        payload = dict(
            schema='fvp-ui-rects/1',
            source='直接读 .bin 归档里的 44 字节 HZC1 头（不解包、不落像素）',
            games={g: dict(rects=v['rects'], missing=v['missing']) for g, v in out.items()})
        with open(a.json, 'w', encoding='utf-8') as f:
            json.dump(payload, f, ensure_ascii=False, indent=1)
        print('json -> %s' % a.json)
    return 0


if __name__ == '__main__':
    sys.exit(main())