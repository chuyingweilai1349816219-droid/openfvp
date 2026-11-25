#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从资源包里静态提取**标题菜单的图元矩形**（不跑原版、不截图）。

为什么：第 122 轮确认标题画面是脚本画的、菜单项就是图元、命中靠 `PrimHit`。
那"按钮位置分布"就等于这些图元的矩形，而图元的矩形来自**素材头**——脚本用
`GraphLoad` 把图装进槽位后没有 `PrimSetXY`（第 122 轮实测），位置来自 HZC1 头里
自带的 `x/y/w/h`（这也是 §4.6「素材头里自带摆放坐标」的同一机制）。

本脚本直接读原版 `graph.bin` 的 BIN 索引（格式见 docs/c-assets.md §3），对
`title_*` 条目读出各自 HZC1 头的 44 字节，得到 `x, y, w, h`，再推出：
  - 每一项的**按钮矩形**（= PrimHit 判定的那个矩形）
  - 右边缘 = x + w（原版是右对齐：5 项应当同一条右边缘）
  - 行距 = 相邻两项的 y 差（应当等于 100 / 90）

只输出名字/矩形/推导量，不含像素数据。

用法：
    python scripts/extract-title-rects.py                 # 打印 + 写 ref/title-rects.json
    python scripts/extract-title-rects.py --check         # 只比对（门禁用）
    python scripts/extract-title-rects.py --game hoshimemo
"""
import argparse
import hashlib
import io
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAMES = {
    'iroseka': os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD'),
    'hoshimemo': os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed'),
}
# 按顺序在这些包里找 title_* 条目（一般都在 graph.bin）
PACKAGES = ['graph.bin', 'graph_bg.bin', 'graph_bs.bin']
OUT_JSON = os.path.join(ROOT, 'ref', 'title-rects.json')
ITEMS = ['title_start', 'title_continue', 'title_album', 'title_option', 'title_end']


def read_index(path):
    """读 BIN 索引 → [(name, data_off, size)]，只解 Shift-JIS 名字。"""
    with open(path, 'rb') as fp:
        head = fp.read(8)
        if len(head) < 8:
            return None
        count, name_len = struct.unpack('<II', head)
        if count == 0 or count > 100000:
            return None
        idx = fp.read(12 * count)
        if len(idx) < 12 * count:
            return None
        names = fp.read(name_len)
        if len(names) < name_len:
            return None
    out = []
    for i in range(count):
        name_off, data_off, size = struct.unpack_from('<III', idx, i * 12)
        if name_off >= name_len:
            continue
        end = names.find(b'\0', name_off)
        if end < 0:
            continue
        raw = names[name_off:end]
        try:
            name = raw.decode('cp932')
        except UnicodeDecodeError:
            name = raw.decode('cp932', 'replace')
        out.append((name, data_off, size))
    return out


def hzc1_rect(path, off, size):
    """读某条数据的 HZC1 头 → (w, h, x, y)；不是 HZC1 返回 None。"""
    if size < 44:
        return None
    with open(path, 'rb') as fp:
        fp.seek(off)
        hdr = fp.read(44)
    if len(hdr) < 44 or hdr[:4] != b'hzc1' or hdr[12:16] != b'NVSG':
        return None
    w, h, x, y = struct.unpack_from('<4H', hdr, 20)
    return {'w': w, 'h': h, 'x': x, 'y': y}


def scan_game(folder):
    for pkg in PACKAGES:
        path = os.path.join(folder, pkg)
        if not os.path.isfile(path):
            continue
        index = read_index(path)
        if index is None:
            continue
        rows = {}
        for name, off, size in index:
            if not name.startswith('title_'):
                continue
            rect = hzc1_rect(path, off, size)
            if rect is None:
                continue
            rect['name'] = name
            rect['right'] = rect['x'] + rect['w']
            rect['bottom'] = rect['y'] + rect['h']
            rows[name] = rect
        if rows:
            rows['_package'] = pkg
            return rows
    return None


def summarize(rows):
    items = {}
    for base in ITEMS:
        if base not in rows:
            continue
        r = rows[base]
        variants = sorted(n for n in rows
                          if n.startswith(base) and n != base
                          and n[0] != '_')
        items[base] = {
            'rect': {k: r[k] for k in ('x', 'y', 'w', 'h', 'right', 'bottom')},
            'variants': {v: {k: rows[v][k] for k in ('x', 'y', 'w', 'h')}
                         for v in variants},
        }
    ys = [items[b]['rect']['y'] for b in ITEMS if b in items]
    rights = sorted({items[b]['rect']['right'] for b in ITEMS if b in items})
    pitch = sorted({ys[i + 1] - ys[i] for i in range(len(ys) - 1)}) if len(ys) > 1 else []
    return {
        'items': items,
        'rows_y': ys,
        'row_pitch': pitch,
        'distinct_right_edges': rights,
        'all_right_aligned': len(rights) == 1,
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default='all', choices=['all'] + sorted(GAMES))
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)

    names = sorted(GAMES) if opt.game == 'all' else [opt.game]
    result = {
        'note': '标题菜单按钮矩形（来自素材 HZC1 头的 x/y/w/h）；'
                '由 scripts/extract-title-rects.py 生成，不含像素数据',
        'games': {},
    }
    for name in names:
        folder = GAMES[name]
        if not os.path.isdir(folder):
            print('SKIP: %s 不存在（%s）' % (name, folder))
            return 0
        rows = scan_game(folder)
        if rows is None:
            print('SKIP: %s 里没有找到 title_* 素材' % name)
            return 0
        pkg = rows.pop('_package')
        summary = summarize(rows)
        summary['package'] = pkg
        summary['asset_count'] = len(rows)
        result['games'][name] = summary
        print('%s  包=%s  素材=%d 项' % (name, pkg, len(rows)))
        for base, it in summary['items'].items():
            r = it['rect']
            print('   %-16s %4dx%-3d @(%5d,%4d)  right=%d  变体 %d 个'
                  % (base, r['w'], r['h'], r['x'], r['y'], r['right'],
                     len(it['variants'])))
        print('   行 y = %s，行距 = %s，右边缘 = %s（统一=%s）'
              % (summary['rows_y'], summary['row_pitch'],
                 summary['distinct_right_edges'], summary['all_right_aligned']))

    text = json.dumps(result, ensure_ascii=False, indent=1, sort_keys=True) + '\n'
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        with io.open(OUT_JSON, encoding='utf-8') as fp:
            old = fp.read()
        if old != text:
            print('FAIL: 提取结果与 ref/title-rects.json 不一致')
            import difflib
            for line in list(difflib.unified_diff(
                    old.splitlines(), text.splitlines(), 'ref', 'new',
                    lineterm=''))[:40]:
                print(line)
            return 1
        print('OK: 与 ref/title-rects.json 一致')
        return 0
    with io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write(text)
    print('写入 %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())