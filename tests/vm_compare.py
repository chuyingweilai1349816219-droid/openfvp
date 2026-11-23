#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""vm_compare.py -- compare the end-to-end C VM+renderer frame with the
published Python reference frame.

The frame written by fvp/tests/render_vm.c (bin/render_vm.exe) is the canvas of
the real C script VM with the C renderer attached, captured at a checkpoint.
This tool compares it with fvp/out/render/story_<step>.png using the same
statistics as tests/gfx_compare.py (imported, not copied): geometry from the
draw list, differing pixels, PSNR/SSIM, per-sprite and text-area split.

Outputs (default fvp/out/c):
    vm-diff.json   full report
    vm-diff.png    false-colour difference heat map (x8)

Usage:
    python fvp/tests/vm_compare.py
    python fvp/tests/vm_compare.py --frame out/c/vm-frame.png --geom out/c/vm-geom.json
    python fvp/tests/vm_compare.py --frame out/c/vm-frame-nohold.png --out out/c/nohold --quiet
"""
import argparse
import json
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)  # fvp/
sys.path.insert(0, HERE)

import gfx_compare  # noqa: E402


def main(argv=None):
    ap = argparse.ArgumentParser(description='compare the C VM render with the '
                                             'Python reference frame')
    ap.add_argument('--frame', default=os.path.join(ROOT, 'out', 'c',
                                                    'vm-frame.png'))
    ap.add_argument('--geom', default=os.path.join(ROOT, 'out', 'c',
                                                   'vm-geom.json'))
    ap.add_argument('--scene', default=os.path.join(ROOT, 'out', 'c',
                                                    'gfx-scene.json'),
                    help='reference scene JSON (draw list + text windows)')
    ap.add_argument('--ref', default=None,
                    help='reference PNG (default: out/render/story_<step>.png)')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'c'))
    ap.add_argument('--diff-name', default='vm-diff')
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
        print('vm_compare: reference PNG not found: %s' % ref_path,
              file=sys.stderr)
        return 2
    os.makedirs(a.out, exist_ok=True)

    frame = Image.open(a.frame).convert('RGBA')
    ref = Image.open(ref_path).convert('RGBA')
    if frame.size != ref.size:
        print('vm_compare: size mismatch %s vs %s' % (frame.size, ref.size),
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
    prem_c = C[..., :3].astype(np.float64) * C[..., 3:4] / 255.0
    prem_r = R[..., :3].astype(np.float64) * R[..., 3:4] / 255.0
    mse_prem = float(np.mean(np.abs(prem_c - prem_r) ** 2))
    psnr_prem = (10.0 * np.log10(255.0 ** 2 / mse_prem)) if mse_prem > 0 else None

    geo = gfx_compare.compare_geometry(scene, geom)

    # text/dialogue boxes: same rule as gfx_compare.py
    lay = scene.get('text_layout') or {}
    text_boxes = []
    for t in scene.get('texts') or []:
        if not t.get('lines'):
            continue
        L = lay.get(str(t.get('idx'))) or dict(x=120, y=880, maxw=1600,
                                               size=30, lines=3)
        x = L['x']
        y = L['y']
        if t.get('pos'):
            x, y = t['pos']
        if int(t['idx']) == 9:
            x = 0
        size = L['size']
        if int(t['idx']) == 0:
            size = max(18, min(40, t['size'][1] if t['size'][1] > 8 else 34))
        text_boxes.append((t['idx'], gfx_compare.clip_rect(
            x - 40, y - 20, L['maxw'] + 80, size * L['lines'] + 60, W, H)))

    regions = []
    for (name, x, y, w, h) in gfx_compare.sprite_regions(scene):
        box = gfx_compare.clip_rect(x, y, w, h, W, H)
        if box is None:
            continue
        x0, y0, x1, y1 = box
        sub = d_rgb[y0:y1, x0:x1]
        regions.append(dict(name=name, box=[x0, y0, x1, y1],
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

    vis = np.clip(d_rgb.astype(np.float64) * 8.0, 0, 255).astype(np.uint8)
    heat = np.zeros((H, W, 3), dtype=np.uint8)
    heat[..., 0] = vis
    heat[..., 1] = (vis // 3).astype(np.uint8)
    heat[..., 2] = (vis // 6).astype(np.uint8)
    diff_png = os.path.join(a.out, a.diff_name + '.png')
    Image.fromarray(heat, 'RGB').save(diff_png)

    report = dict(
        frame=os.path.abspath(a.frame),
        reference=os.path.abspath(ref_path),
        scene=os.path.abspath(a.scene),
        geom=os.path.abspath(a.geom),
        step=int(geom.get('step', scene.get('step', 0))),
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
            ssim_luma=round(gfx_compare.ssim_gray(
                gfx_compare.luma(C), gfx_compare.luma(R)), 6),
        ),
        regions=regions,
        text_area=dict(
            boxes=[b for (_i, b) in text_boxes if b is not None],
            differing_px=text_diff,
            outside_differing_px=outside_diff,
            outside_max_diff=outside_max),
        diff_image=os.path.abspath(diff_png),
    )
    path = os.path.join(a.out, a.diff_name + '.json')
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(report, f, ensure_ascii=False, indent=1)

    if not a.quiet:
        p = report['pixels']
        print('=== end-to-end VM render comparison ===')
        print('frame       %s' % a.frame)
        print('reference   %s  (%dx%d, step %d)' %
              (ref_path, W, H, report['step']))
        print('geometry    %d/%d sprites identical, position error %s px' %
              (geo['c_count'], geo['reference_count'],
               '0' if geo['position_error_px'] == 0 else 'NON-ZERO'))
        print('pixels      %d differing (%.4f%%), rgb>8: %d, max diff %d' %
              (p['differing_rgb'], p['percent_differing_rgb'],
               p['differing_rgb_gt8'], p['max_rgb_diff']))
        print('quality     PSNR(rgb) %.2f dB, PSNR(on black) %.2f dB, '
              'SSIM(luma) %.5f' %
              (p['psnr_rgb_db'] or 0.0, p['psnr_on_black_db'] or 0.0,
               p['ssim_luma']))
        print('text area   %d differing px inside text boxes, %d outside '
              '(max %d)' % (text_diff, outside_diff, outside_max))
        for r in regions:
            if r['diff_px']:
                print('  region %-38s %6d px differ (max %3d)' %
                      (r['name'], r['diff_px'], r['max_diff']))
        print('report      %s' % path)
        print('heatmap     %s' % diff_png)
    return 0


if __name__ == '__main__':
    sys.exit(main())