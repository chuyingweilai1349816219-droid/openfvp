#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""标题进场（逐元素脚本时间轴）的**端到端**门禁。

单元测试 `tests/test_titleenter.c` 证明时间轴数学；门禁
`tests/verify_title_enter.py` 证明 C 里的常量 == 脚本；这里把外壳焊上：

  A. `--title-anim --shot-at 2600`  进场中（logo 半透明 + 菜单逐项开始）
  D. `--title-anim --shot-at 1500`  进场早期（logo 还没开始 = 只有标题背景）
  B. 不开动画的静态标题（基线口径）
  C. `--title-anim` 跑到 4600 ms   进场结束（4000 ms 后）

断言：
  1. **A != B**：逐元素淡入确实在画面上生效；
  2. **D 与静态的差 > A 与静态的差**：早期只有背景，logo/菜单/版权行都还看不见，
     越到后面越接近静态 —— 这是"错开进场"的端到端证据；
  3. **C == B 逐字节相同**：4000 ms 后逐元素 alpha 全 255、偏移全 0，与静态一致；
  4. 日志：A/D/C 都有 `script entrance started`，B 没有。

缺 `bin/fvp.exe` 或游戏目录时 SKIP 并返回 0（和 verify_opening_render.py 一致）。

用法：python tests/verify_title_anim_render.py
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from verify_vs_original import bmp_pixels  # noqa: E402

EXE = os.path.join(FVP, 'bin', 'fvp.exe')
OUT = os.path.join(FVP, 'out', 'c')
FONT = os.path.join(FVP, 'bin', 'gfx-font-61.bin')
# 每部作品的抓图时刻（外壳用墙钟；进场从标题显示时刻起算，误差 ~0.2 s）：
#   mid   = 进场中（logo 半透明 + 菜单开始错开）
#   early = 进场早期（只有标题背景）
#   done  = 进场结束之后（五彩 4000 ms / 星空 3000 ms + 余量）
GAMES = {
    'iroseka': {
        'dir': r'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD',
        'script': 'iroseka.hcb',
        'mid': 2600, 'early': 1500, 'done': 4600, 'run': 5.2,
    },
    'hoshimemo': {
        'dir': r'C:\Users\Mechrevo\Downloads\hoshimemo_installed',
        'script': 'Hoshimemo_HD.hcb',
        'mid': 2000, 'early': 800, 'done': 3800, 'run': 4.4,
    },
}

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def run_shell(game_dir, script, shot, log, extra, seconds):
    args = [EXE, '--no-input', '--game', game_dir, '--script', script,
            '--no-audio', '--scale', '1', '--title-menu', '--title-font', FONT,
            '--title-no-hover',
            '--run-seconds', '%g' % seconds, '--screenshot', shot, '--log', log]
    args += extra
    try:
        return subprocess.run(args, timeout=300).returncode
    except subprocess.TimeoutExpired:
        return 1


def diff(a, b):
    total = 0.0
    n = 0
    changed = 0
    for y in range(min(len(a), len(b))):
        ra = a[y]
        rb = b[y]
        for x in range(min(len(ra), len(rb))):
            pa = ra[x]
            pb = rb[x]
            d = abs(pa[0] - pb[0]) + abs(pa[1] - pb[1]) + abs(pa[2] - pb[2])
            total += d
            n += 3
            if d > 24:
                changed += 1
    if n == 0:
        return 0.0, 0.0
    return total / n, 100.0 * changed / (len(a[0]) * len(a))


def read_log(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as fp:
            return fp.read()
    except OSError:
        return ''


def check_game(name, g):
    shots = {k: os.path.join(OUT, 'titleenter-%s-%s.bmp' % (name, k))
             for k in ('mid', 'early', 'off', 'done')}
    logs = {k: os.path.join(OUT, 'titleenter-%s-%s.log' % (name, k))
            for k in ('mid', 'early', 'off', 'done')}
    plan = [
        ('mid', ['--title-anim', '--shot-at', str(g['mid'])], g['run'] * 0.6),
        ('early', ['--title-anim', '--shot-at', str(g['early'])],
         g['early'] / 1000.0 + 0.8),
        ('off', [], 1.6),
        ('done', ['--title-anim', '--shot-at', str(g['done'])], g['run']),
    ]
    print('==== %s ==' % name)
    for key, extra, seconds in plan:
        for pth in (shots[key], logs[key]):
            if os.path.isfile(pth):
                os.remove(pth)
        rc = run_shell(g['dir'], g['script'], shots[key], logs[key], extra,
                       seconds)
        check(rc == 0 and os.path.isfile(shots[key]),
              '%s/%s: 外壳退出码 0 且写出截图（rc=%s）' % (name, key, rc))
    if fails:
        return
    mid = bmp_pixels(shots['mid'])
    early = bmp_pixels(shots['early'])
    off = bmp_pixels(shots['off'])
    done = bmp_pixels(shots['done'])
    if not check(all(x is not None for x in (mid, early, off, done)),
                 '%s: 四张 BMP 都读得出来' % name):
        return

    print('-- 1. 进场中 vs 不开动画 --')
    m_ab, p_ab = diff(mid, off)
    print('  %s mean_abs=%.3f changed=%.2f%%' % (name, m_ab, p_ab))
    check(m_ab > 0.5, '%s: 进场中 mean_abs > 0.5（%.3f）' % (name, m_ab))
    check(p_ab > 1.0, '%s: 差异像素 > 1%%（%.2f%%）' % (name, p_ab))

    print('-- 2. 早期只有背景（比进场中更偏离静态）--')
    e_off, _ = diff(early, off)
    print('  %s early vs static mean_abs=%.3f' % (name, e_off))
    check(e_off > m_ab,
          '%s: 早期与静态的差 > 进场中与静态的差（%.3f > %.3f）'
          % (name, e_off, m_ab))

    print('-- 3. 进场跑完 == 静态帧（逐字节）--')
    m_cb, p_cb = diff(done, off)
    print('  %s mean_abs=%.6f changed=%.4f%%' % (name, m_cb, p_cb))
    check(m_cb == 0.0, '%s: 进场终点 mean_abs == 0（%.6f）' % (name, m_cb))
    check(p_cb == 0.0, '%s: 没有差异像素（%.4f%%）' % (name, p_cb))

    print('-- 4. 日志 --')
    for key in ('mid', 'early', 'off', 'done'):
        text = read_log(logs[key])
        got = 'script entrance started' in text
        if key == 'off':
            check(not got, '%s/off 没有进场日志' % name)
        else:
            check(got, '%s/%s 有脚本进场日志' % (name, key))
    return m_ab


def main():
    if not os.path.isfile(EXE):
        print('SKIP: bin/fvp.exe 不存在（未构建 win32 外壳）')
        return 0
    if not os.path.isdir(OUT):
        os.makedirs(OUT)
    have = []
    for name in sorted(GAMES):
        g = GAMES[name]
        if not os.path.isdir(g['dir']) or \
                not os.path.isfile(os.path.join(g['dir'], g['script'])):
            print('SKIP %s: 缺游戏目录/脚本 %s' % (name, g['dir']))
            continue
        have.append(name)
    if not have:
        print('SKIP: 没有可用的游戏数据')
        return 0
    means = {}
    for name in have:
        check_game(name, GAMES[name])
        means[name] = means.get(name, 0.0)
    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 两个作品的逐元素进场都在画面上生效、早期只有背景、'
          '进场结束后与静态帧逐字节相同')
    return 0


if __name__ == '__main__':
    sys.exit(main())