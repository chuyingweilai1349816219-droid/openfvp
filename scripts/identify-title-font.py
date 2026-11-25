"""Identify the serif font used by iroseka's title menu.

The original title menu draws "New Game / Continue / Album / Option / Exit" in a
dark-navy serif face.  This tool renders candidates from the local Windows font
directory, aligns them to the ink box measured from the original title frame and
reports the normalized correlation, so the choice is evidence-based instead of a
guess.

Usage:
    python scripts/identify-title-font.py
    python scripts/identify-title-font.py --frame out/orig-title/orig-title-005.png

Read-only: needs the original title frame captured by
`scripts/capture-original.ps1` (kept under `out/`, never committed).
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)

# 原版截图是 1926x1109 的窗口抓帧，客户区左上角在 (3,29)
FRAME_OFFSET = (3, 29)
# iroseka 第 1 项 "New Game" 的实测墨迹框（客户区坐标）
INK = (1461, 1769, 511, 552)
LABEL = 'New Game'

CANDIDATES = [
    ('georgiab.ttf', 'Georgia Bold'),
    ('georgia.ttf', 'Georgia'),
    ('bookos.ttf', 'Bookman Old Style'),
    ('pala.ttf', 'Palatino Linotype'),
    ('palab.ttf', 'Palatino Linotype Bold'),
    ('palabi.ttf', 'Palatino Linotype Bold Italic'),
    ('constan.ttf', 'Constantia'),
    ('cambria.ttc', 'Cambria'),
    ('times.ttf', 'Times New Roman'),
    ('timesbd.ttf', 'Times New Roman Bold'),
    ('GARA.TTF', 'Garamond'),
    ('msmincho.ttc', 'MS Mincho'),
    ('msgothic.ttc', 'MS Gothic'),
]


def norm(m):
    m = m - m.mean()
    n = np.sqrt((m * m).sum())
    return m / n if n > 0 else m


def match(ref, path, label, size, search=8):
    try:
        font = ImageFont.truetype(path, size)
    except Exception:
        return None
    base = Image.new('L', ref.shape[::-1], 255)
    ImageDraw.Draw(base).text((24, 24), LABEL, font=font, fill=0)
    m0 = (np.asarray(base, dtype=np.float64) < 128).astype(np.float64)
    ys, xs = np.nonzero(m0)
    if len(xs) == 0:
        return None
    oy, ox = np.nonzero(ref)
    dy0 = int(round(oy.mean() - ys.mean()))
    dx0 = int(round(ox.mean() - xs.mean()))
    best = (-1.0, 0, 0)
    for dy in range(dy0 - search, dy0 + search + 1):
        for dx in range(dx0 - search, dx0 + search + 1):
            m = np.zeros_like(m0)
            ys2 = ys + dy
            xs2 = xs + dx
            ok = ((ys2 >= 0) & (ys2 < m.shape[0]) & (xs2 >= 0) &
                  (xs2 < m.shape[1]))
            m[ys2[ok], xs2[ok]] = 1.0
            score = float((norm(m) * norm(ref)).sum())
            if score > best[0]:
                best = (score, dx, dy)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--frame', default=os.path.join(
        FVP, 'out', 'orig-title', 'orig-title-005.png'))
    ap.add_argument('--font-dir', default=r'C:\Windows\Fonts')
    ap.add_argument('--sizes', default='50:62')
    args = ap.parse_args()

    if not os.path.isfile(args.frame):
        print('identify-title-font: missing frame %s (capture it first)'
              % args.frame, file=sys.stderr)
        return 1
    lo, hi = (int(x) for x in args.sizes.split(':'))
    img = Image.open(args.frame).convert('RGB')
    img = img.crop((FRAME_OFFSET[0], FRAME_OFFSET[1],
                    FRAME_OFFSET[0] + 1920, FRAME_OFFSET[1] + 1080))
    crop = img.crop((INK[0] - 24, INK[2] - 24, INK[1] + 24, INK[3] + 24))
    ref = (np.asarray(crop.convert('L'), dtype=np.float64) < 110).astype(
        np.float64)
    print('reference: %d ink px in a %dx%d box' % (int(ref.sum()),
                                                   crop.size[0], crop.size[1]))

    results = []
    for fname, name in CANDIDATES:
        path = os.path.join(args.font_dir, fname)
        if not os.path.isfile(path):
            continue
        for size in range(lo, hi + 1):
            r = match(ref, path, LABEL, size)
            if r is not None:
                results.append((r[0], name, size, r[1], r[2]))
    results.sort(reverse=True)
    for score, name, size, dx, dy in results[:12]:
        print('%6.3f  %-22s size %2d  shift (%+d,%+d)' % (score, name, size,
                                                          dx, dy))
    if not results:
        print('no candidate fonts found under %s' % args.font_dir)
        return 1
    print('\nbest: %s size %d (correlation %.3f)' % (results[0][1],
                                                     results[0][2],
                                                     results[0][0]))
    # 尺寸旁证：参考墨迹框 vs 最佳候选渲染出来的墨迹框
    oxs = np.nonzero(ref)[1]
    oys = np.nonzero(ref)[0]
    ref_box = (int(oxs.max() - oxs.min()) + 1, int(oys.max() - oys.min()) + 1)
    path = None
    for fname, name in CANDIDATES:
        if name == results[0][1]:
            path = os.path.join(args.font_dir, fname)
            break
    if path is not None:
        try:
            font = ImageFont.truetype(path, results[0][2])
            img2 = Image.new('L', ref.shape[::-1], 255)
            ImageDraw.Draw(img2).text((24, 24), LABEL, font=font, fill=0)
            m = np.asarray(img2, dtype=np.float64) < 128
            xs = np.nonzero(m)[1]
            ys = np.nonzero(m)[0]
            box = (int(xs.max() - xs.min()) + 1, int(ys.max() - ys.min()) + 1)
            print('ink box: reference %dx%d, candidate %dx%d'
                  % (ref_box[0], ref_box[1], box[0], box[1]))
        except Exception:
            pass
    return 0


if __name__ == '__main__':
    sys.exit(main())