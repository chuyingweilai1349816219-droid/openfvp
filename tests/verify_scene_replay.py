#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_scene_replay.py -- 验证"FVP v2 场景回放"和引擎实时渲染画出同一帧。

链路：

  1. `bin/fvp-headless.exe <script> --steps N --dump-scene scene.bin --screenshot a.bmp`
     —— C 引擎跑到第 N 步，导出场景 + 当时画布（24 位 BMP）。
  2. `bin/fvp.exe --game <dir> --script <hcb> --scene scene.bin --run-seconds 1.5
     --screenshot b.bmp`
     —— 桌面外壳把同一份场景读回 gfx 重画（32 位 BMP）。
  3. 逐像素比较 a.bmp 和 b.bmp：应当完全一致（同一份 src/gfx.c、同一批图元/图槽）。

意义：这是"离线帧 == 窗口帧"的直接证据（目标清单 ② 的要求）。若两边不一致，
说明 `FVPS` 场景格式或外壳侧装载丢/改了状态。

缺 exe / 游戏数据时 SKIP 并返回 0。
"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, 'out', 'c')
GAME = os.path.join(r'C:\Users\Mechrevo\Downloads',
                    '\u4e94\u5f69\u6591\u6593\u7684\u4e16\u754cHD')
SCRIPT = os.path.join(GAME, 'iroseka.hcb')
HEADLESS = os.path.join(ROOT, 'bin', 'fvp-headless.exe')
SHELL = os.path.join(ROOT, 'bin', 'fvp.exe')
STEPS = 30000


def read_bmp(path):
    """读 24/32 位 BMP，返回 (w, h, rows[y][x] = (r,g,b))。"""
    with open(path, 'rb') as fp:
        data = fp.read()
    if data[:2] != b'BM':
        return None
    off = struct.unpack_from('<I', data, 10)[0]
    w, h = struct.unpack_from('<ii', data, 18)
    bpp = struct.unpack_from('<H', data, 28)[0]
    if bpp not in (24, 32) or w <= 0 or h == 0:
        return None
    top_down = h < 0
    h = abs(h)
    stride = ((w * bpp // 8) + 3) & ~3
    rows = []
    for y in range(h):
        src = y if top_down else (h - 1 - y)
        row = off + src * stride
        px = []
        for x in range(w):
            p = row + x * (bpp // 8)
            px.append((data[p + 2], data[p + 1], data[p]))
        rows.append(px)
    return w, h, rows


def compare(a, b):
    if a is None or b is None:
        return None
    wa, ha, ra = a
    wb, hb, rb = b
    if (wa, ha) != (wb, hb):
        return dict(size_mismatch=True, wa=wa, ha=ha, wb=wb, hb=hb)
    changed = 0
    total = 0
    maxd = 0
    for y in range(ha):
        pa = ra[y]
        pb = rb[y]
        for x in range(wa):
            ca = pa[x]
            cb = pb[x]
            d = abs(ca[0] - cb[0]) + abs(ca[1] - cb[1]) + abs(ca[2] - cb[2])
            total += 1
            if d > 24:
                changed += 1
            if d > maxd:
                maxd = d
    return dict(size_mismatch=False, changed=changed, total=total, maxd=maxd)


def main():
    if not os.path.isfile(HEADLESS) or not os.path.isfile(SHELL):
        print('SKIP: bin/fvp-headless.exe or bin/fvp.exe missing')
        return 0
    if not os.path.isfile(SCRIPT):
        print('SKIP: game data missing')
        return 0
    os.makedirs(OUT, exist_ok=True)
    scene = os.path.join(OUT, 'scene-replay.bin')
    shot_a = os.path.join(OUT, 'scene-replay-live.bmp')
    shot_b = os.path.join(OUT, 'scene-replay-replay.bmp')
    for p in (scene, shot_a, shot_b):
        if os.path.isfile(p):
            os.remove(p)

    env = dict(os.environ)
    env['FVP_GAME_DIR'] = GAME
    rc = subprocess.call([HEADLESS, SCRIPT, '--steps', str(STEPS), '--quiet',
                          '--dump-scene', scene, '--screenshot', shot_a],
                         env=env, timeout=300)
    if rc != 0 or not os.path.isfile(scene) or not os.path.isfile(shot_a):
        print('FAIL: headless dump/scene failed (rc=%s)' % rc)
        return 1

    rc = subprocess.call([SHELL, '--game', GAME, '--script', 'iroseka.hcb',
                          '--no-audio', '--scale', '1', '--scene', scene,
                          '--run-seconds', '1.5', '--screenshot', shot_b,
                          '--log', os.path.join(OUT, 'scene-replay.log')],
                         timeout=300)
    if rc != 0 or not os.path.isfile(shot_b):
        print('FAIL: shell scene replay failed (rc=%s)' % rc)
        return 1

    a = read_bmp(shot_a)
    b = read_bmp(shot_b)
    if a is None or b is None:
        print('FAIL: cannot read the two BMPs (%s, %s)' % (a is None, b is None))
        return 1
    r = compare(a, b)
    print('live   %dx%d, replay %dx%d' % (a[0], a[1], b[0], b[1]))
    if r['size_mismatch']:
        print('FAIL: size mismatch')
        return 1
    pct = 100.0 * r['changed'] / r['total']
    print('pixels %d/%d changed (%.4f%%), max channel diff %d'
          % (r['changed'], r['total'], pct, r['maxd']))
    if r['maxd'] != 0:
        print('FAIL: replay frame != live frame')
        return 1
    print('\nOK: --scene 回放与引擎实时渲染的同一帧逐像素一致')
    return 0


if __name__ == '__main__':
    sys.exit(main())