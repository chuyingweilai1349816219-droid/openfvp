#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""标题进场动画的**端到端**门禁：动画真的跑、跑完真的与静态帧逐字节相同。

单元测试（`tests/test_gfx_xform.c`）证明了"xform 在 scale=1000 时与静态绘制字节等价"，
门禁 `tests/verify_title_anim.py` 证明了"C 里的参数 == 脚本常量"。这里把**外壳**也焊上：
真的启动 `bin/fvp.exe` 跑标题，抓三张图对比：

  A. `--title-anim --run-seconds 0.6`  动画中途（logo alpha 0 附近、10 倍缩放）
  B. `--run-seconds 3`                 不开动画（静态，基线口径）
  C. `--title-anim --run-seconds 3`     动画跑完

断言：
  1. **A != B**：动画确实在画面上生效（mean_abs 明显 > 0，且差异像素占比够大）；
  2. **C == B 逐字节相同**：补间结束时 alpha=255 / scale=1000，画出来与不做动画
     完全一样 —— 这是"默认关 + 动画终点等于静态"两条性质合起来的证明，
     也是冻结基线（`verify_vs_original`）不会被动画影响的前提；
  3. 日志：A/C 有 "entrance animation started"，B 没有。

缺 `bin/fvp.exe` 或游戏目录时 SKIP 并返回 0（和 `verify_vs_original` 一致）。

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
        rc = subprocess.run(args, timeout=300).returncode
    except subprocess.TimeoutExpired:
        return 1
    return rc


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

    shots = {k: os.path.join(OUT, 'titleanim-%s.bmp' % k)
             for k in ('mid', 'off', 'done')}
    logs = {k: os.path.join(OUT, 'titleanim-%s.log' % k)
            for k in ('mid', 'off', 'done')}

    plan = [
        ('mid', ['--title-anim'], 0.6),
        ('off', [], 3.0),
        ('done', ['--title-anim'], 3.0),
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
    off = bmp_pixels(shots['off'])
    done = bmp_pixels(shots['done'])
    check(mid is not None and off is not None and done is not None,
          '三张 BMP 都读得出来')

    print('== 1. 动画中途 vs 不开动画：画面必须明显不同 ==')
    m_ab, p_ab = diff(mid, off)
    print('  mean_abs=%.3f  changed pixels=%.2f%%' % (m_ab, p_ab))
    check(m_ab > 2.0, '动画中途的 mean_abs > 2.0（实际 %.3f）' % m_ab)
    check(p_ab > 5.0, '差异像素占比 > 5%%（实际 %.2f%%）' % p_ab)

    print('== 2. 动画跑完 vs 不开动画：必须逐字节相同 ==')
    m_cb, p_cb = diff(done, off)
    print('  mean_abs=%.6f  changed pixels=%.4f%%' % (m_cb, p_cb))
    check(m_cb == 0.0, '动画终点的 mean_abs == 0（实际 %.6f）' % m_cb)
    check(p_cb == 0.0, '动画终点没有一个差异像素（实际 %.4f%%）' % p_cb)

    print('== 3. 日志：只有开动画的两次打出 "entrance animation started" ==')
    for key in ('mid', 'off', 'done'):
        text = read_log(logs[key])
        got = 'entrance animation started' in text
        if key == 'off':
            check(not got, 'off 那次没有动画日志')
        else:
            check(got, '%s 那次有动画日志' % key)

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 进场动画在画面上生效（mean_abs %.2f），且跑完后与静态帧逐字节相同'
          % m_ab)
    return 0


if __name__ == '__main__':
    sys.exit(main())