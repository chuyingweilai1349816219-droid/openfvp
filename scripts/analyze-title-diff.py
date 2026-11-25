#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把"和原版标题帧差 13.13"拆到**区域**上：差在 logo、菜单、版权行还是背景？

为什么做这个：总平均差是个标量，看不出下一步该修哪里。把同一张原版帧按标题画面的
实际构图切成四块（都用**素材头里的矩形**，不是手画框），就能知道误差集中在谁身上。
这一步属于 B 类（最终像素验证），但结论不是用来"调参"，而是用来**排下一轮的优先级**。

区域（矩形全部来自 ref/title-rects.json 同源的素材头）：
  logo      `title_logo1`      1150x283 @ (701,149)
  menu      5 个菜单项贴图的并集（五彩斑斓 1445..1785 x 495..985）
  copyright `title_copyright`   590x37  @ (1189,1043)
  rest      其余部分

产物 `ref/title-diff-regions.json`（只含统计）。

用法（需要 Pillow/numpy，用 DSH 运行时 Python）：
    python scripts/analyze-title-diff.py            # 打印 + 写 json
    python scripts/analyze-title-diff.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAMES = {
    'iroseka': os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD'),
}
BASELINE = os.path.join(ROOT, 'tests', 'original-baseline.json')
OUT_JSON = os.path.join(ROOT, 'ref', 'title-diff-regions.json')
OUT_DIR = os.path.join(ROOT, 'out', 'title-diff')

# 五彩斑斓标题画面的区域（素材头原值）
REGIONS = {
    'logo': (701, 149, 1150, 283),
    'menu': (1445, 495, 340, 490),
    'copyright': (1189, 1043, 590, 37),
}


def read_png(path, box=None):
    import numpy as np
    from PIL import Image

    im = Image.open(path).convert('RGB')
    if box is not None:
        im = im.crop((box[0], box[1], box[0] + box[2], box[1] + box[3]))
    return np.asarray(im, dtype=np.int16)


def read_bmp(path, box=None):
    import numpy as np

    with open(path, 'rb') as fp:
        data = fp.read()
    off = int.from_bytes(data[10:14], 'little')
    w = int.from_bytes(data[18:22], 'little', signed=True)
    h = int.from_bytes(data[22:26], 'little', signed=True)
    bpp = int.from_bytes(data[28:30], 'little')
    if bpp != 32:
        raise SystemExit('unexpected BMP bpp %d' % bpp)
    stride = w * 4
    rows = []
    for y in range(h):
        row = off + (h - 1 - y) * stride        # BMP 自下而上
        px = data[row:row + stride]
        arr = np.frombuffer(px, dtype=np.uint8).reshape(w, 4)[:, 2::-1]
        rows.append(arr)
    img = np.stack(rows).astype(np.int16)
    if box is not None:
        img = img[box[1]:box[1] + box[3], box[0]:box[0] + box[2]]
    return img


def diff_stats(a, b, step=2):
    """与 tests/verify_vs_original.py 同口径：隔 2 像素取一个点，避免采样差异。"""
    import numpy as np

    if a.shape != b.shape:
        return None
    a2 = a[::step, ::step]
    b2 = b[::step, ::step]
    d = np.abs(a2 - b2)
    return {
        'px': int(a2.shape[0] * a2.shape[1]),
        'diff_px': int((d.max(axis=2) > 0).sum()),
        'mean_abs': round(float(d.mean()), 4),
        'max_channel': int(d.max()),
    }


def run_shell(game_dir, shot, log, extra=()):
    shell = os.path.join(ROOT, 'bin', 'fvp.exe')
    if not os.path.isfile(shell):
        return False
    for p in (shot, log):
        if os.path.isfile(p):
            os.remove(p)
    subprocess.run([shell, '--no-input', '--game', game_dir, '--script',
                    'iroseka.hcb', '--run-seconds', '3', '--no-audio',
                    '--scale', '1', '--screenshot', shot,
                    '--log', log] + list(extra),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=120)
    return os.path.isfile(shot)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default='iroseka', choices=sorted(GAMES))
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)

    game_dir = GAMES[opt.game]
    with io.open(BASELINE, encoding='utf-8') as fp:
        cases = {c['name']: c for c in json.load(fp)['cases']}
    case = cases.get('%s-title' % opt.game)
    if case is None or not os.path.isdir(game_dir):
        print('SKIP: 缺基线用例或游戏目录')
        return 0
    frame = os.path.join(ROOT, case['frame'])
    if not os.path.isfile(frame):
        print('SKIP: 缺原版帧 %s' % frame)
        return 0

    os.makedirs(OUT_DIR, exist_ok=True)
    shot = os.path.join(OUT_DIR, '%s-title.bmp' % opt.game)
    log = os.path.join(OUT_DIR, '%s-title.log' % opt.game)
    if not run_shell(game_dir, shot, log, case.get('args') or ()):
        print('SKIP: 外壳没产出截图')
        return 0

    ox, oy = case['offset']
    regions = {}
    total = None
    for name, box in sorted(REGIONS.items()):
        a = read_png(frame, (box[0] + ox, box[1] + oy, box[2], box[3]))
        b = read_bmp(shot, box)
        st = diff_stats(a, b)
        if st is not None:
            regions[name] = st
    # 整体（客户区 1920x1080）
    a = read_png(frame, (ox, oy, 1920, 1080))
    b = read_bmp(shot)
    total = diff_stats(a, b)
    if total is not None:
        covered = sum(r['px'] for r in regions.values())
        rest = {
            'px': total['px'] - covered,
            'diff_px': total['diff_px'] - sum(r['diff_px'] for r in regions.values()),
            'mean_abs': None,
        }
        regions['rest'] = rest

    result = {
        'note': '标题画面与原版帧的分区域差异（区域矩形来自素材头）；'
                '由 scripts/analyze-title-diff.py 生成',
        'game': opt.game,
        'total': total,
        'regions': regions,
    }
    print('%s  total mean_abs=%s  diff_px=%s/%s'
          % (opt.game, total['mean_abs'], total['diff_px'], total['px']))
    for name in sorted(regions):
        st = regions[name]
        print('   %-10s mean_abs=%-8s diff_px=%s/%s'
              % (name, st['mean_abs'], st['diff_px'], st['px']))

    text = json.dumps(result, ensure_ascii=False, indent=1, sort_keys=True) + '\n'
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        with io.open(OUT_JSON, encoding='utf-8') as fp:
            old = fp.read()
        if old != text:
            print('FAIL: 与 %s 不一致（渲染变了？）' % OUT_JSON)
            import difflib
            for line in list(difflib.unified_diff(
                    old.splitlines(), text.splitlines(), 'ref', 'new',
                    lineterm=''))[:40]:
                print(line)
            return 1
        print('OK: 与 ref/title-diff-regions.json 一致')
        return 0
    with io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write(text)
    print('写入 %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())