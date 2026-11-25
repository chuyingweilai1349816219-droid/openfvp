#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""用**素材 + 原版帧**反推标题背景四层的合成模型（不猜、只验证假设）。

背景四层（A/B/C/D，2400×1350 设计画布，裁 240/135 到 1920×1080）都是带 alpha 的
BGRA 图。我们的渲染器按"直通 alpha 的 over 合成"画它们，但原版是 D3D9 的混合，
未必同一模型。本脚本把**几种候选模型**都在 numpy 里算一遍，拿原版标题帧的
"纯背景区域"（扣掉 logo / 菜单 / 版权行）比平均差——哪个模型明显更好就说明
原版用的是哪一种。

候选维度：
  顺序：by-z-ascending（D,C,B,A）／by-z-descending（A,B,C,D）／measured（A,C,D,B）
  混合：over（straight alpha）／premul（预乘 alpha）／add（加色）
  底色：黑 / 白 / 不透明（第一层直接覆盖）

产物 `ref/title-bg-blend.json`（只含统计）。

用法（需要 Pillow+numpy，用 DSH 运行时 Python）：
    python scripts/analyze-title-bg-blend.py [--check]
"""
import argparse
import io
import json
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD')
FRAME = os.path.join(ROOT, 'out', 'orig-title', 'orig-title-005.png')
OUT_JSON = os.path.join(ROOT, 'ref', 'title-bg-blend.json')
LAYERS = ['title_bg1_A', 'title_bg1_B', 'title_bg1_C', 'title_bg1_D']
CROP = (240, 135)          # 设计画布 2400x1350 → 屏幕 1920x1080
W, H = 1920, 1080
# 纯背景区域 = 整屏扣掉 logo / 菜单 / 版权行（矩形来自素材头）
EXCLUDE = [(701, 149, 1150, 283), (1445, 495, 340, 490), (1189, 1043, 590, 37)]

ORDERS = {
    'z-ascending': ['title_bg1_D', 'title_bg1_C', 'title_bg1_B', 'title_bg1_A'],
    'z-descending': ['title_bg1_A', 'title_bg1_B', 'title_bg1_C', 'title_bg1_D'],
    'measured': ['title_bg1_A', 'title_bg1_C', 'title_bg1_D', 'title_bg1_B'],
}
BLENDS = ['over', 'premul', 'add']


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


def load_layer(index, path, name):
    """返回 (rgba float32 [h,w,4] 0..255, x, y)。"""
    import numpy as np

    off, size = index[name]
    with open(path, 'rb') as fp:
        fp.seek(off)
        blob = fp.read(size)
    if blob[:4] != b'hzc1':
        raise SystemExit('not hzc1: %s' % name)
    w, h, x, y = struct.unpack_from('<4H', blob, 0x14)
    fmt = struct.unpack_from('<H', blob, 0x12)[0]
    raw = zlib.decompress(blob[44:])
    bpp = {0: 3, 1: 4, 2: 4, 3: 1, 4: 1}.get(fmt, 4)
    px = np.frombuffer(raw, dtype=np.uint8).reshape(h, w, bpp).astype(np.float32)
    if bpp == 3:
        # fmt 0 = BGR24（没有 alpha）→ 视为完全不透明
        arr = np.empty((h, w, 4), dtype=np.float32)
        arr[..., 0:3] = px
        arr[..., 3] = 255.0
    else:
        arr = px
    return arr, x, y


def place(canvas, img, x, y):
    """把图层按素材头坐标贴到 1920x1080 画布（减去设计画布裁剪量）。"""
    import numpy as np

    dx = x - CROP[0]
    dy = y - CROP[1]
    h, w = img.shape[:2]
    sx0 = max(0, -dx)
    sy0 = max(0, -dy)
    sx1 = min(w, W - dx)
    sy1 = min(h, H - dy)
    if sx1 <= sx0 or sy1 <= sy0:
        return
    dst = canvas[dy + sy0:dy + sy1, dx + sx0:dx + sx1]
    dst[...] = img[sy0:sy1, sx0:sx1]


def composite(layers, order, blend, base):
    import numpy as np

    canvas = np.zeros((H, W, 4), dtype=np.float32)
    canvas[..., 0:3] = base
    canvas[..., 3] = 255.0
    for name in order:
        img = layers[name]
        canvas = _over(canvas, img, blend)
    return canvas


def _over(dst, src, blend):
    """把整幅 src 贴到 dst 上（src 已经按坐标放好尺寸一致）。"""
    import numpy as np

    a = src[..., 3:4] / 255.0
    s = src[..., 0:3]
    d = dst[..., 0:3]
    if blend == 'over':
        out = s * a + d * (1.0 - a)
    elif blend == 'premul':
        # 预乘：源颜色已经乘过 alpha，直接叠加
        out = s + d * (1.0 - a)
    else:  # add
        out = np.clip(d + s * a, 0, 255)
    dst[..., 0:3] = out
    return dst


def mean_abs(ours, theirs, mask):
    import numpy as np

    d = np.abs(ours - theirs)
    d = d[mask]
    return float(d.mean()) if d.size else 0.0


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)

    try:
        import numpy as np
        from PIL import Image
    except ImportError:
        print('SKIP: 缺 Pillow/numpy')
        return 0
    if not os.path.isdir(GAME) or not os.path.isfile(FRAME):
        print('SKIP: 缺游戏目录或原版帧')
        return 0

    pkg = os.path.join(GAME, 'graph.bin')
    index = read_index(pkg)
    layers = {}
    for name in LAYERS:
        img, x, y = load_layer(index, pkg, name)
        canvas = np.zeros((H, W, 4), dtype=np.float32)
        place(canvas, img, x, y)
        layers[name] = canvas

    im = Image.open(FRAME).convert('RGB').crop((3, 29, 3 + W, 29 + H))
    theirs = np.asarray(im, dtype=np.float32)

    mask = np.ones((H, W), dtype=bool)
    for (x, y, w, h) in EXCLUDE:
        mask[y:y + h, x:x + w] = False

    rows = []
    for order_name, order in ORDERS.items():
        for blend in BLENDS:
            for base_name, base in (('black', 0.0), ('white', 255.0)):
                canvas = composite(layers, order, blend, base)
                # 素材是 BGRA，PIL 读出来是 RGB → 比之前翻转通道
                rgb = canvas[..., 0:3][..., ::-1]
                m = mean_abs(rgb, theirs, mask)
                rows.append({'order': order_name, 'blend': blend,
                             'base': base_name, 'mean_abs': round(m, 3)})
    rows.sort(key=lambda r: r['mean_abs'])

    result = {
        'note': '标题背景四层合成模型的候选扫描（用原版帧的纯背景区域验证）；'
                '由 scripts/analyze-title-bg-blend.py 生成',
        'region_px': int(mask.sum()),
        'candidates': rows,
        'best': rows[0],
    }
    print('纯背景区域 %d px；候选 %d 个' % (result['region_px'], len(rows)))
    for r in rows[:8]:
        print('   %-12s %-6s base=%-5s mean_abs=%.3f'
              % (r['order'], r['blend'], r['base'], r['mean_abs']))

    text = json.dumps(result, ensure_ascii=False, indent=1, sort_keys=True) + '\n'
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        with io.open(OUT_JSON, encoding='utf-8') as fp:
            old = fp.read()
        if old != text:
            print('FAIL: 与 %s 不一致' % OUT_JSON)
            return 1
        print('OK: 与 ref/title-bg-blend.json 一致')
        return 0
    with io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write(text)
    print('写入 %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())