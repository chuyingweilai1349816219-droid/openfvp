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
GAMES = {
    'iroseka': (r'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD', 'iroseka.hcb'),
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


def main():
    game = GAMES['iroseka']
    if not os.path.isfile(EXE):
        print('SKIP: bin/fvp.exe 不存在（未构建 win32 外壳）')
        return 0
    if not os.path.isdir(game[0]):
        print('SKIP: 缺游戏目录 %s' % game[0])
        return 0
    if not os.path.isdir(OUT):
        os.makedirs(OUT)

    shots = {k: os.path.join(OUT, 'titleenter-%s.bmp' % k)
             for k in ('mid', 'early', 'off', 'done')}
    logs = {k: os.path.join(OUT, 'titleenter-%s.log' % k)
            for k in ('mid', 'early', 'off', 'done')}

    plan = [
        ('mid', ['--title-anim', '--shot-at', '2600'], 3.2),
        ('early', ['--title-anim', '--shot-at', '1500'], 2.1),
        ('off', [], 1.6),
        ('done', ['--title-anim', '--shot-at', '4600'], 5.2),
    ]
    for key, extra, seconds in plan:
        for p in (shots[key], logs[key]):
            if os.path.isfile(p):
                os.remove(p)
        rc = run_shell(game[0], game[1], shots[key], logs[key], extra, seconds)
        check(rc == 0 and os.path.isfile(shots[key]),
              '%s: 外壳退出码 0 且写出截图（rc=%s）' % (key, rc))

    if fails:
        print('\nFAIL: 外壳没跑起来')
        return 1

    mid = bmp_pixels(shots['mid'])
    early = bmp_pixels(shots['early'])
    off = bmp_pixels(shots['off'])
    done = bmp_pixels(shots['done'])
    check(all(x is not None for x in (mid, early, off, done)),
          '四张 BMP 都读得出来')

    print('== 1. 进场中 vs 不开动画：画面必须明显不同 ==')
    m_ab, p_ab = diff(mid, off)
    print('  mean_abs=%.3f  changed pixels=%.2f%%' % (m_ab, p_ab))
    check(m_ab > 0.5, '进场中的 mean_abs > 0.5（实际 %.3f）' % m_ab)
    check(p_ab > 1.0, '差异像素占比 > 1%%（实际 %.2f%%）' % p_ab)

    print('== 2. 早期 vs 进场中：早期更接近"只有背景" ==')
    e_off, ep_off = diff(early, off)
    print('  early vs static: mean_abs=%.3f' % e_off)
    check(e_off > m_ab,
          '早期与静态的差 > 进场中与静态的差（%.3f > %.3f，错开进场生效）'
          % (e_off, m_ab))

    print('== 3. 进场跑完 vs 不开动画：必须逐字节相同 ==')
    m_cb, p_cb = diff(done, off)
    print('  mean_abs=%.6f  changed pixels=%.4f%%' % (m_cb, p_cb))
    check(m_cb == 0.0, '进场终点的 mean_abs == 0（实际 %.6f）' % m_cb)
    check(p_cb == 0.0, '进场终点没有一个差异像素（实际 %.4f%%）' % p_cb)

    print('== 4. 日志：只有开动画的几次打出 "script entrance started" ==')
    for key in ('mid', 'early', 'off', 'done'):
        text = read_log(logs[key])
        got = 'script entrance started' in text
        if key == 'off':
            check(not got, 'off 那次没有进场日志')
        else:
            check(got, '%s 那次有脚本进场日志' % key)

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 逐元素进场在画面上生效（mean_abs %.2f），早期只有背景，'
          '4000 ms 后与静态帧逐字节相同' % m_ab)
    return 0


if __name__ == '__main__':
    sys.exit(main())