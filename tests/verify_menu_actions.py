#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""游戏内菜单的动作门禁（SAVE / LOAD / OPTION）。

上一轮只验证了"菜单能打开、有 5 项、有墨迹"；这一轮把三个动作做成真动作，这里逐个验证：

  SAVE   -> 用 --save-template + --save-preview 写一份**真实模板存档**到 out/，
            绝不写进游戏自己的 save/ 目录；断言文件存在、大小 > 500 KB、时间戳是今年。
  LOAD   -> 打开存档列表；断言日志报出的可读槽数与磁盘上的 sNNN.bin 数一致。
  OPTION -> 打开只读设置面板；断言日志 + 截图下半部分有面板文字墨迹。

用法：python tests/verify_menu_actions.py
缺游戏数据返回 2（SKIP）。
"""
import glob
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
OUT = os.path.join(FVP, 'out', 'menuact')
GAME = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD')


def run(argv, timeout=900):
    proc = subprocess.run(argv, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout)
    return proc.returncode, proc.stdout.decode('utf-8', 'replace')


def read_log(path):
    if not os.path.isfile(path):
        return ''
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


def bmp_ink(path, x0, y0, x1, y1, thresh=120):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] != b'BM':
        return -1
    off = struct.unpack_from('<I', data, 10)[0]
    w = struct.unpack_from('<i', data, 18)[0]
    h = struct.unpack_from('<i', data, 22)[0]
    bpp = struct.unpack_from('<H', data, 28)[0]
    if bpp != 32:
        return -1
    stride = w * 4
    n = 0
    for y in range(y0, y1, 3):
        row = off + (h - 1 - y) * stride  # BMP 是自下而上存的
        for x in range(x0, x1, 3):
            p = row + x * 4
            if data[p] + data[p + 1] + data[p + 2] > thresh:
                n += 1
    return n


def main():
    exe = os.path.join(FVP, 'bin', 'fvp.exe')
    script = os.path.join(GAME, 'iroseka.hcb')
    saves = sorted(glob.glob(os.path.join(GAME, 'save', 's[0-9][0-9][0-9].bin')))
    if not os.path.isfile(exe) or not os.path.isfile(script) or not saves:
        print('verify_menu_actions: SKIP (missing binary / game data)')
        return 2
    os.makedirs(OUT, exist_ok=True)
    fails = []

    def check(cond, msg):
        print(('  ok    ' if cond else '  FAIL  ') + msg)
        if not cond:
            fails.append(msg)

    font = os.path.join(FVP, 'bin', 'gfx-font-61.bin')
    extra = ['--title-font', font] if os.path.isfile(font) else []
    base = [exe, '--no-input', '--game', GAME, '--script', 'iroseka.hcb',
            '--run-seconds', '3', '--no-audio', '--scale', '1',
            '--menu-at', '300']

    # --- 1. SAVE：写真实模板存档到 out/，不碰游戏 save/ ---
    tmpl = saves[0]
    out_save = os.path.join(OUT, 'menu-save.bin')
    log1 = os.path.join(OUT, 'save.log')
    for p in (out_save, log1):
        if os.path.isfile(p):
            os.remove(p)
    rc, _o = run(base + ['--menu-pick', '0', '--save-template', tmpl,
                         '--save-preview', out_save, '--log', log1] + extra)
    check(rc == 0, 'SAVE run exit 0 (got %s)' % rc)
    text = read_log(log1)
    check('menu: SAVE ->' in text, 'SAVE action logged')
    check('game save dir untouched' in text,
          'SAVE says the game save dir is untouched')
    check('menu: auto-pick item 0' in text, 'SAVE auto-picked')
    if os.path.isfile(out_save):
        size = os.path.getsize(out_save)
        with open(out_save, 'rb') as f:
            head = f.read(7)
        year = struct.unpack_from('<H', head, 0)[0]
        check(size > 500000, 'SAVE wrote a real slot (%d bytes)' % size)
        check(year >= 2024, 'SAVE slot timestamp is current (year %d)' % year)
    else:
        check(False, 'SAVE produced a file')

    # --- 2. LOAD：打开存档列表，槽数与磁盘一致 ---
    log2 = os.path.join(OUT, 'load.log')
    if os.path.isfile(log2):
        os.remove(log2)
    rc, _o = run(base + ['--menu-pick', '1', '--log', log2] + extra)
    check(rc == 0, 'LOAD run exit 0 (got %s)' % rc)
    text = read_log(log2)
    m = re.search(r'menu: LOAD -> save list \((\d+) slot\(s\)\)', text)
    check(m is not None, 'LOAD opened the save list (%s)'
          % (m.group(0) if m else 'missing'))
    if m:
        check(int(m.group(1)) == len(saves),
              'LOAD found %d slot(s), disk has %d' % (int(m.group(1)),
                                                      len(saves)))

    # --- 3. OPTION：只读面板 + 有墨迹 ---
    log3 = os.path.join(OUT, 'option.log')
    shot3 = os.path.join(OUT, 'option.bmp')
    for p in (log3, shot3):
        if os.path.isfile(p):
            os.remove(p)
    rc, _o = run(base + ['--menu-pick', '2', '--menu-adjust', '-3',
                         '--log', log3, '--screenshot', shot3] + extra)
    check(rc == 0, 'OPTION run exit 0 (got %s)' % rc)
    text = read_log(log3)
    check('menu: OPTION panel opened' in text, 'OPTION panel opened')
    check('menu: master volume -> 90%' in text,
          'OPTION adjusts master volume (100 -> 90)')
    check('menu: master volume -> 70%' in text,
          'OPTION reaches 70% after three -10 steps')
    if os.path.isfile(shot3):
        ink = bmp_ink(shot3, 110, 740, 900, 940)
        check(ink > 200, 'OPTION panel has text ink (%d samples)' % ink)
    else:
        check(False, 'OPTION screenshot written')

    if fails:
        print('verify_menu_actions: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_menu_actions: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())