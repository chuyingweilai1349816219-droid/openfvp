#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""静态比对标题菜单的**状态变体**像素，定出"哪个变体是 hovered 态"。

为什么做这个：五彩斑斓的菜单项素材有 5 个状态变体（`title_start1..5` 这种），
但脚本只按名字 `GraphLoad`，**没有任何字段说明哪个变体对应哪种状态**。
按方法论，这件事要在资源包里解决：把变体解出来逐像素比，看差异落在哪里、
差多少——不靠游戏截图猜。

做法：
  1. 读 `graph.bin` 的 BIN 索引，取 `title_<item>` 与 `title_<item>1..5`；
  2. 每条按 **HZC1**（44 字节头 + zlib，见 docs/hzc1-format.md）解成 BGRA；
  3. 以基准态（`title_<item>`）为参照，算每个变体的：
     - 差异像素数 / 占比 / 差异包围盒 / 最大通道差
     - "变体里比基准更亮/更不透明"的像素数（hover 通常是发光/提亮）
  4. 汇总：对每个变体给出它在 5 个菜单项上的**一致行为**（如果 5 项都表现一致，
     那它就是同一个状态），从而定出映射。

产物 `ref/title-variants.json`（只含统计，不含像素数据）。

用法：
    python scripts/analyze-title-variants.py            # 打印 + 写 json
    python scripts/analyze-title-variants.py --check    # 只比对（门禁用）
"""
import argparse
import hashlib
import io
import json
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAMES = {
    'iroseka': os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD'),
    'hoshimemo': os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed'),
}
OUT_JSON = os.path.join(ROOT, 'ref', 'title-variants.json')
ITEMS = ['title_start', 'title_continue', 'title_album', 'title_option',
         'title_end']
PKG = 'graph.bin'
BPP = {0: 3, 1: 4, 2: 4, 3: 1, 4: 1}


def read_index(path):
    with open(path, 'rb') as fp:
        head = fp.read(8)
        count, name_len = struct.unpack('<II', head)
        idx = fp.read(12 * count)
        names = fp.read(name_len)
    out = {}
    for i in range(count):
        name_off, data_off, size = struct.unpack_from('<III', idx, i * 12)
        if name_off >= name_len:
            continue
        end = names.find(b'\0', name_off)
        if end < 0:
            continue
        try:
            name = names[name_off:end].decode('cp932')
        except UnicodeDecodeError:
            continue
        out[name] = (data_off, size)
    return out


def decode_hzc1(blob):
    """返回 (w, h, fmt, raw_bytes)；不是 hzc1 返回 None。"""
    if len(blob) < 44 or blob[:4] != b'hzc1' or blob[12:16] != b'NVSG':
        return None
    w, h, x, y = struct.unpack_from('<4H', blob, 0x14)
    fmt = struct.unpack_from('<H', blob, 0x12)[0]
    try:
        raw = zlib.decompress(blob[44:])
    except zlib.error:
        return None
    return {'w': w, 'h': h, 'fmt': fmt, 'raw': raw,
            'bpp': BPP.get(fmt, 4)}


def compare(base, var):
    """逐像素比（只比前 base.bpp 个通道；尺寸不同则以 base 为画布裁剪）。"""
    bw, bh, bpp = base['w'], base['h'], base['bpp']
    vw, vh = var['w'], var['h']
    b = base['raw']
    v = var['raw']
    vbpp = var['bpp']
    diff = 0
    gain = 0          # 变体比基准更亮（含 alpha 更高）
    loss = 0
    x0 = y0 = None
    x1 = y1 = None
    maxd = 0
    total = bw * bh
    for y in range(bh):
        for x in range(bw):
            bi = (y * bw + x) * bpp
            if x >= vw or y >= vh:
                diff += 1
                continue
            vi = (y * vw + x) * vbpp
            d = 0
            for c in range(min(bpp, vbpp)):
                d = max(d, abs(b[bi + c] - v[vi + c]))
            if d > 0:
                diff += 1
                maxd = max(maxd, d)
                if x0 is None or x < x0:
                    x0 = x
                if y0 is None or y < y0:
                    y0 = y
                if x1 is None or x > x1:
                    x1 = x
                if y1 is None or y > y1:
                    y1 = y
                # 亮度（BGR 求和）或 alpha 变大 → 更"发光"
                bl = sum(b[bi + c] for c in range(min(3, bpp)))
                vl = sum(v[vi + c] for c in range(min(3, vbpp)))
                if vl > bl:
                    gain += 1
                elif vl < bl:
                    loss += 1
    return {
        'diff_px': diff,
        'diff_pct': round(100.0 * diff / total, 3) if total else 0.0,
        'bbox': None if x0 is None else [x0, y0, x1, y1],
        'max_channel_delta': maxd,
        'brighter_px': gain,
        'darker_px': loss,
    }


def mean_rgba(img):
    """整图均值（BGRA 四通道，取整）。用来判断"高亮态"与"淡出帧"。"""
    raw, bpp = img['raw'], img['bpp']
    n = img['w'] * img['h']
    if n <= 0:
        return [0, 0, 0, 0]
    sb = sg = sr = sa = 0
    for i in range(n):
        b, g, r, a = raw[i * bpp:i * bpp + 4]
        sb += b
        sg += g
        sr += r
        sa += a
    return [sb // n, sg // n, sr // n, sa // n]


def analyze(folder):
    path = os.path.join(folder, PKG)
    if not os.path.isfile(path):
        return None
    index = read_index(path)
    rows = {}
    for item in ITEMS:
        names = [item] + ['%s%d' % (item, k) for k in range(1, 6)]
        present = [n for n in names if n in index]
        if item not in index:
            continue
        with open(path, 'rb') as fp:
            off, size = index[item]
            fp.seek(off)
            base = decode_hzc1(fp.read(size))
        if base is None:
            continue
        entry = {'base': {'w': base['w'], 'h': base['h'],
                          'fmt': base['fmt'],
                          'mean_bgra': mean_rgba(base)},
                 'variants': {}}
        for n in present:
            if n == item:
                continue
            with open(path, 'rb') as fp:
                off, size = index[n]
                fp.seek(off)
                var = decode_hzc1(fp.read(size))
            if var is None:
                continue
            st = compare(base, var)
            st['w'] = var['w']
            st['h'] = var['h']
            st['fmt'] = var['fmt']
            st['mean_bgra'] = mean_rgba(var)
            entry['variants'][n] = st
        entry['base']['present_variants'] = sorted(entry['variants'])
        rows[item] = entry
    return rows


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default='all', choices=['all'] + sorted(GAMES))
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)

    names = sorted(GAMES) if opt.game == 'all' else [opt.game]
    result = {
        'note': '标题菜单状态变体与基准态的逐像素差异统计（仅统计量）；'
                '由 scripts/analyze-title-variants.py 生成，不含像素数据',
        'games': {},
    }
    for name in names:
        folder = GAMES[name]
        if not os.path.isdir(folder):
            print('SKIP: %s 不存在（%s）' % (name, folder))
            return 0
        rows = analyze(folder)
        if rows is None:
            print('SKIP: %s 没有 %s' % (name, PKG))
            return 0
        result['games'][name] = rows
        print('%s：%d 个菜单项的变体比对' % (name, len(rows)))
        for item in ITEMS:
            e = rows.get(item)
            if not e:
                continue
            print('  %s 基准 %dx%d，变体 %d 个'
                  % (item, e['base']['w'], e['base']['h'],
                     len(e['variants'])))
            for vn, st in sorted(e['variants'].items()):
                print('     %-18s diff %5d px (%6.2f%%)  brighter %5d  '
                      'darker %5d  maxdelta %3d  bbox %s'
                      % (vn, st['diff_px'], st['diff_pct'],
                         st['brighter_px'], st['darker_px'],
                         st['max_channel_delta'], st['bbox']))

    text = json.dumps(result, ensure_ascii=False, indent=1, sort_keys=True) + '\n'
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        with io.open(OUT_JSON, encoding='utf-8') as fp:
            old = fp.read()
        if old != text:
            print('FAIL: 结果与 %s 不一致' % OUT_JSON)
            import difflib
            for line in list(difflib.unified_diff(
                    old.splitlines(), text.splitlines(), 'ref', 'new',
                    lineterm=''))[:40]:
                print(line)
            return 1
        print('OK: 与 ref/title-variants.json 一致')
        return 0
    with io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write(text)
    print('写入 %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())