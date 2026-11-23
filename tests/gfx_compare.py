#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gfx_compare.py -- pixel + geometry comparison for the C gfx renderer.

Compares
    fvp/out/c/gfx-frame.png    (written by bin/test_gfx.exe)
  against
    fvp/out/render/story_00528000.png  (the proven Python reference frame)

and the C draw list (gfx-geom.json) against the reference draw list stored in
gfx-scene.json (expect_draw).  Writes fvp/out/c/gfx-diff.json and a
false-colour difference image fvp/out/c/gfx-diff.png.

Usage:
    python fvp/tests/gfx_compare.py
    python fvp/tests/gfx_compare.py --frame x.png --scene y.json --out fvp/out/c
"""
import argparse
import json
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)  # fvp/


# ---------------------------------------------------------------------------
# SSIM (grayscale, 11x11 Gaussian window, sigma 1.5 -- the usual constants)
# ---------------------------------------------------------------------------
def _gauss_kernel(size=11, sigma=1.5):
    ax = np.arange(size, dtype=np.float64) - (size - 1) / 2.0
    k = np.exp(-(ax ** 2) / (2.0 * sigma ** 2))
    return k / k.sum()


def _filt2(img, k):
    """Separable 'same' convolution with edge replication."""
    r = len(k) // 2
    h, w = img.shape
    tmp = np.zeros_like(img)
    pad = np.pad(img, ((0, 0), (r, r)), mode='edge')
    for i, wt in enumerate(k):
        tmp += wt * pad[:, i:i + w]
    out = np.zeros_like(img)
    pad = np.pad(tmp, ((r, r), (0, 0)), mode='edge')
    for i, wt in enumerate(k):
        out += wt * pad[i:i + h, :]
    return out


def ssim_gray(a, b):
    a = a.astype(np.float64)
    b = b.astype(np.float64)
    k = _gauss_kernel()
    mu_a = _filt2(a, k)
    mu_b = _filt2(b, k)
    va = _filt2(a * a, k) - mu_a * mu_a
    vb = _filt2(b * b, k) - mu_b * mu_b
    cov = _filt2(a * b, k) - mu_a * mu_b
    c1 = (0.01 * 255.0) ** 2
    c2 = (0.03 * 255.0) ** 2
    num = (2 * mu_a * mu_b + c1) * (2 * cov + c2)
    den = (mu_a ** 2 + mu_b ** 2 + c1) * (va + vb + c2)
    return float(np.mean(num / np.maximum(den, 1e-12)))


def luma(rgba):
    return (0.299 * rgba[..., 0] + 0.587 * rgba[..., 1] +
            0.114 * rgba[..., 2])


# ---------------------------------------------------------------------------
# geometry comparison
# ---------------------------------------------------------------------------
def compare_geometry(scene, geom):
    exp = scene.get('expect_draw') or []
    got = geom.get('draw') or []
    mism = []
    for i in range(max(len(exp), len(got))):
        e = exp[i] if i < len(exp) else None
        g = got[i] if i < len(got) else None
        if e is None or g is None:
            mism.append(dict(index=i, reason='count mismatch',
                             expected=e, got=g))
            continue
        keys = ('prim', 'name', 'x', 'y', 'w', 'h', 'mode', 'z', 'alpha')
        bad = {k: [e.get(k), g.get(k)] for k in keys if e.get(k) != g.get(k)}
        if bad:
            mism.append(dict(index=i, reason='field mismatch', fields=bad))
    return dict(
        reference_count=len(exp), c_count=len(got), mismatches=mism,
        position_error_px=0 if not mism else None,
        identical=(len(mism) == 0))


# ---------------------------------------------------------------------------
# regions: every drawn sprite plus the text/dialogue areas
# ---------------------------------------------------------------------------
def sprite_regions(scene):
    out = []
    for d in scene.get('expect_draw') or []:
        out.append((d['name'], d['x'], d['y'], d['w'], d['h']))
    return out


def clip_rect(x, y, w, h, cw, ch):
    x0 = max(0, int(x))
    y0 = max(0, int(y))
    x1 = min(cw, int(x) + int(w))
    y1 = min(ch, int(y) + int(h))
    if x1 <= x0 or y1 <= y0:
        return None
    return (x0, y0, x1, y1)


def main(argv=None):
    ap = argparse.ArgumentParser(description='compare the C gfx frame with the '
                                             'Python reference')
    ap.add_argument('--frame', default=os.path.join(ROOT, 'out', 'c',
                                                    'gfx-frame.png'))
    ap.add_argument('--scene', default=os.path.join(ROOT, 'out', 'c',
                                                    'gfx-scene.json'))
    ap.add_argument('--geom', default=os.path.join(ROOT, 'out', 'c',
                                                   'gfx-geom.json'))
    ap.add_argument('--ref', default=None,
                    help='reference PNG (default: out/render/story_<step>.png)')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'c'))
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args(argv)

    with open(a.scene, encoding='utf-8') as f:
        scene = json.load(f)
    with open(a.geom, encoding='utf-8') as f:
        geom = json.load(f)
    ref_path = a.ref
    if ref_path is None:
        ref_path = os.path.join(ROOT, 'out', 'render',
                                'story_%08d.png' % int(scene.get('step', 528000)))
    if not os.path.exists(ref_path):
        print('gfx_compare: reference PNG not found: %s' % ref_path,
              file=sys.stderr)
        return 2

    frame = Image.open(a.frame).convert('RGBA')
    ref = Image.open(ref_path).convert('RGBA')
    if frame.size != ref.size:
        print('gfx_compare: size mismatch %s vs %s' % (frame.size, ref.size),
              file=sys.stderr)
        return 2
    C = np.asarray(frame).astype(np.int32)
    R = np.asarray(ref).astype(np.int32)
    H, W = C.shape[:2]

    d = np.abs(C - R)
    d_rgb = d[..., :3].max(axis=2)
    d_any = d.max(axis=2)
    d_alpha = d[..., 3]

    mse_rgb = float(np.mean((d[..., :3].astype(np.float64)) ** 2))
    psnr_rgb = (10.0 * np.log10(255.0 ** 2 / mse_rgb)) if mse_rgb > 0 else None
    # alpha-premultiplied "what a viewer sees on black"
    prem_c = C[..., :3].astype(np.float64) * C[..., 3:4] / 255.0
    prem_r = R[..., :3].astype(np.float64) * R[..., 3:4] / 255.0
    d_prem = np.abs(prem_c - prem_r)
    mse_prem = float(np.mean(d_prem ** 2))
    psnr_prem = (10.0 * np.log10(255.0 ** 2 / mse_prem)) if mse_prem > 0 else None

    geo = compare_geometry(scene, geom)

    # text/dialogue areas: the layout boxes of every text window that drew
    lay = scene.get('text_layout') or {}
    text_boxes = []
    for t in scene.get('texts') or []:
        if not t.get('lines'):
            continue
        L = lay.get(str(t.get('idx'))) or dict(x=120, y=880, maxw=1600, size=30,
                                               lines=3)
        x = L['x']
        y = L['y']
        if t.get('pos'):
            x, y = t['pos']
        if int(t['idx']) == 9:
            x = 0
        size = L['size']
        if int(t['idx']) == 0:
            size = max(18, min(40, t['size'][1] if t['size'][1] > 8 else 34))
        text_boxes.append((t['idx'], clip_rect(x - 40, y - 20,
                                             L['maxw'] + 80,
                                             size * L['lines'] + 60, W, H)))

    regions = []
    for (name, x, y, w, h) in sprite_regions(scene):
        box = clip_rect(x, y, w, h, W, H)
        if box is None:
            continue
        x0, y0, x1, y1 = box
        sub = d_rgb[y0:y1, x0:x1]
        regions.append(dict(name=name, prim=None, box=[x0, y0, x1, y1],
                            diff_px=int((sub > 0).sum()),
                            max_diff=int(sub.max()) if sub.size else 0,
                            mean_diff=float(sub.mean()) if sub.size else 0.0))
    text_union = np.zeros((H, W), dtype=bool)
    for (_idx, box) in text_boxes:
        if box is not None:
            x0, y0, x1, y1 = box
            text_union[y0:y1, x0:x1] = True
    text_diff = int((d_rgb[text_union] > 0).sum())
    outside_diff = int((d_rgb[~text_union] > 0).sum())
    outside_max = int(d_rgb[~text_union].max())

    # false-colour difference image (amplified 8x, red where it differs)
    vis = np.clip(d_rgb.astype(np.float64) * 8.0, 0, 255).astype(np.uint8)
    heat = np.zeros((H, W, 3), dtype=np.uint8)
    heat[..., 0] = vis
    heat[..., 1] = (vis // 3).astype(np.uint8)
    heat[..., 2] = (vis // 6).astype(np.uint8)
    diff_png = os.path.join(a.out, 'gfx-diff.png')
    Image.fromarray(heat, 'RGB').save(diff_png)

    report = dict(
        frame=os.path.abspath(a.frame),
        reference=os.path.abspath(ref_path),
        scene=os.path.abspath(a.scene),
        step=int(scene.get('step', 0)),
        size=[W, H],
        geometry=dict(
            reference_sprites=geo['reference_count'],
            c_sprites=geo['c_count'],
            identical=geo['identical'],
            position_error_px=geo['position_error_px'],
            mismatches=geo['mismatches'][:20]),
        pixels=dict(
            total=W * H,
            differing_any_channel=int((d_any > 0).sum()),
            differing_rgb=int((d_rgb > 0).sum()),
            differing_alpha=int((d_alpha > 0).sum()),
            differing_rgb_gt8=int((d_rgb > 8).sum()),
            differing_rgb_gt32=int((d_rgb > 32).sum()),
            percent_differing_rgb=round(100.0 * (d_rgb > 0).mean(), 4),
            max_channel_diff=int(d.max()),
            max_rgb_diff=int(d_rgb.max()),
            mean_abs_diff_rgb=round(float(d[..., :3].mean()), 6),
            mse_rgb=round(mse_rgb, 6),
            psnr_rgb_db=None if psnr_rgb is None else round(psnr_rgb, 3),
            psnr_on_black_db=None if psnr_prem is None else round(psnr_prem, 3),
            ssim_luma=round(ssim_gray(luma(C), luma(R)), 6),
        ),
        regions=regions,
        text_area=dict(
            boxes=[b for (_i, b) in text_boxes if b is not None],
            differing_px=text_diff,
            outside_differing_px=outside_diff,
            outside_max_diff=outside_max),
        diff_image=os.path.abspath(diff_png),
    )
    path = os.path.join(a.out, 'gfx-diff.json')
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(report, f, ensure_ascii=False, indent=1)

    if not a.quiet:
        p = report['pixels']
        print('=== gfx pixel comparison ===')
        print('frame      %s' % a.frame)
        print('reference  %s  (%dx%d, step %d)' %
              (ref_path, W, H, report['step']))
        print('geometry   %d/%d sprites identical, position error %s px' %
              (geo['c_count'], geo['reference_count'],
               '0' if geo['position_error_px'] == 0 else 'NON-ZERO'))
        print('pixels     %d differing (%.4f%%), rgb>8: %d, max diff %d' %
              (p['differing_rgb'], p['percent_differing_rgb'],
               p['differing_rgb_gt8'], p['max_rgb_diff']))
        print('quality    PSNR(rgb) %.2f dB, PSNR(on black) %.2f dB, '
              'SSIM(luma) %.5f' %
              (p['psnr_rgb_db'] or 0.0, p['psnr_on_black_db'] or 0.0,
               p['ssim_luma']))
        print('text area  %d differing px inside text boxes, %d outside '
              '(max %d)' %
              (text_diff, outside_diff, outside_max))
        for r in regions:
            if r['diff_px']:
                print('  region %-38s %6d px differ (max %3d)' %
                      (r['name'], r['diff_px'], r['max_diff']))
        print('report     %s' % path)
        print('heatmap    %s' % diff_png)
    return 0


if __name__ == '__main__':
    sys.exit(main())