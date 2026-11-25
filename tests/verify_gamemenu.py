#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""游戏内菜单（引擎侧 UI）门禁。

原版的游戏内菜单由 exe 自己画（脚本只有 MenuMessSkip / TitleMenu 两条相关命令），
所以它和标题菜单一样属于外壳实现。这个门禁用 `--menu-at N` 在脚本运行后打开菜单，
断言：

  1. 日志里有 `menu: auto-opened at <t> ms (5 item(s))`，且 t 落在合理区间；
  2. 截图里菜单区域（画面中部）确实有文字墨迹（不是空菜单）；
  3. 关闭时日志有 `menu: closed`（用 `--menu-at` + 到点后自动打开一次，再跑一遍带
     `--menu-close-at` 的路径太重；这里只验证打开态）。

缺游戏数据时返回 2（SKIP）。
"""
import argparse
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
OUT = os.path.join(FVP, 'out', 'gamemenu')
DEFAULT_GAME = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD')


def run(argv, timeout=900):
    proc = subprocess.run(argv, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout)
    return proc.returncode, proc.stdout.decode('utf-8', 'replace')


def read_bmp(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] != b'BM':
        return None
    off = struct.unpack_from('<I', data, 10)[0]
    w = struct.unpack_from('<i', data, 18)[0]
    h = struct.unpack_from('<i', data, 22)[0]
    bpp = struct.unpack_from('<H', data, 28)[0]
    return data, off, w, h, bpp


def ink_in_box(path, x0, y0, x1, y1, thresh=90):
    got = read_bmp(path)
    if got is None:
        return -1
    data, off, w, h, bpp = got
    if bpp != 32:
        return -1
    stride = w * 4
    count = 0
    for y in range(y0, y1, 3):
        row = off + (h - 1 - y) * stride  # BMP 是自下而上，但我们写的是自上而下
        for x in range(x0, x1, 3):
            p = row + x * 4
            if data[p] + data[p + 1] + data[p + 2] > thresh:
                count += 1
    return count


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=DEFAULT_GAME)
    ap.add_argument('--at', type=int, default=400)
    args = ap.parse_args()
    exe = os.path.join(FVP, 'bin', 'fvp.exe')
    script = os.path.join(args.game, 'iroseka.hcb')
    if not os.path.isfile(exe) or not os.path.isfile(script):
        print('verify_gamemenu: SKIP (missing binary or game data)')
        return 2
    os.makedirs(OUT, exist_ok=True)
    shot = os.path.join(OUT, 'menu.bmp')
    log = os.path.join(OUT, 'menu.log')
    for p in (shot, log):
        if os.path.isfile(p):
            os.remove(p)
    font = os.path.join(FVP, 'bin', 'gfx-font-61.bin')
    extra = ['--title-font', font] if os.path.isfile(font) else []
    rc, out = run([exe, '--no-input', '--game', args.game, '--script',
                   'iroseka.hcb', '--run-seconds', '2', '--no-audio',
                   '--scale', '1', '--menu-at', str(args.at),
                   '--screenshot', shot, '--log', log] + extra)
    fails = []

    def check(cond, msg):
        print(('  ok    ' if cond else '  FAIL  ') + msg)
        if not cond:
            fails.append(msg)

    check(rc == 0, 'shell exit 0 (got %s)' % rc)
    text = ''
    if os.path.isfile(log):
        with open(log, encoding='utf-8', errors='replace') as f:
            text = f.read()
    m = re.search(r'menu: auto-opened at ([\d.]+) ms \((\d+) item\(s\)\)', text)
    check(m is not None, 'menu auto-open logged (%s)'
          % (m.group(0) if m else 'missing'))
    if m:
        at = float(m.group(1))
        items = int(m.group(2))
        check(items == 5, '%d menu items (got %d)' % (5, items))
        check(args.at <= at <= args.at + 800,
              'opened near the requested time (%.0f ms vs %d ms)'
              % (at, args.at))
    got = read_bmp(shot) if os.path.isfile(shot) else None
    check(got is not None, 'screenshot written')
    if got:
        data, off, w, h, bpp = got
        check((w, h) == (1920, 1080) and bpp == 32,
              'screenshot is 1920x1080x32 (%dx%dx%d)' % (w, h, bpp))
        lit = 0
        stride = w * 4
        for y in range(280, 620, 3):
            row = off + y * stride
            for x in range(840, 1120, 3):
                p = row + x * 4
                if data[p] + data[p + 1] + data[p + 2] > 120:
                    lit += 1
        check(lit > 200, 'menu area has text ink (%d samples)' % lit)

    if fails:
        print('verify_gamemenu: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_gamemenu: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())