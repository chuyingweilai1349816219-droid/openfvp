#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""开场 FAVORITE logo 的**端到端**门禁：真的启动外壳，按脚本时间轴抓帧验证。

单元测试（`tests/test_openseq.c`）证明时间轴数学；门禁
`tests/verify_opening_anim.py` 证明 C 里的常量 == 脚本；这里再把外壳焊上：

  A. `--opening --shot-at 250`    开场早期（白底 + logo 淡入中）
  B. `--opening --shot-at 2900`   淡出中（白底快没了，标题背景已经露出来）
  C. `--opening --shot-at 4800`   开场结束（6 层 + 100 ms 收尾后进标题）
  D. 不开 `--opening` 的静态标题（基线口径）

注：开场 3800 ms 结束后标题还有 4000 ms 的逐元素进场（脚本 0x5933E..），
所以 C 要等到 ~8.2 s 才等于静态帧。

断言：
  1. **A 是白底**（近白像素 > 90%）：开场从白场开始；
  2. **A != B**：白底淡出确实发生了（脚本 MotionAlpha(250, keep, 0, 3500)）；
  3. **B 已经能看见标题背景**（蓝分量明显高于红）：脚本先建标题场景
     （hcb 0x590CC call 0x584D8）再跑开场（0x591EC），白底淡出露出的是标题背景，
     不是黑屏。这也是旧实现（白屏→标题遮罩）丢掉的事实；
  4. **C == D 逐字节相同**：开场 3800 ms + 标题进场 4000 ms 之后画面 ==
     静态标题帧 —— 说明开场六层全部归零、标题进场也归位，冻结基线不受影响；
  5. 日志：A/B 打印 `6/6 layers loaded` 且带 `opening t=`；C 打印
     `opening: finished in ... 6 layers`；开场结束**之后**才出现
     `title: script entrance started`（脚本是开场函数返回后才排标题进场）。

缺 `bin/fvp.exe` 或游戏目录时 SKIP 并返回 0（和 verify_title_anim_render.py 一致）。

用法：python tests/verify_opening_render.py
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from verify_vs_original import bmp_pixels  # noqa: E402

EXE = os.path.join(FVP, 'bin', 'fvp.exe')
OUT = os.path.join(FVP, 'out', 'c')
FONT = os.path.join(FVP, 'bin', 'gfx-font-61.bin')
GAME = (r'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD', 'iroseka.hcb')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def run_shell(shot, log, extra, seconds):
    args = [EXE, '--no-input', '--game', GAME[0], '--script', GAME[1],
            '--no-audio', '--scale', '1', '--title-menu', '--title-font', FONT,
            '--run-seconds', '%g' % seconds, '--screenshot', shot, '--log', log]
    args += extra
    try:
        return subprocess.run(args, timeout=300).returncode
    except subprocess.TimeoutExpired:
        return 1


def read_log(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as fp:
            return fp.read()
    except OSError:
        return ''


def mean_abs(a, b):
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


def stats(px):
    """返回 (平均 R,G,B, 近白像素比例)。"""
    n = 0
    sr = sg = sb = 0
    white = 0
    for row in px:
        for p in row:
            sr += p[0]
            sg += p[1]
            sb += p[2]
            if p[0] > 200 and p[1] > 200 and p[2] > 200:
                white += 1
            n += 1
    if n == 0:
        return 0.0, 0.0, 0.0, 0.0
    return sr / n, sg / n, sb / n, white / n


def opening_t(log):
    m = re.search(r'opening t=(\d+) ms', log)
    return int(m.group(1)) if m else None


def main():
    if not os.path.isfile(EXE):
        print('SKIP: bin/fvp.exe 不存在（未构建 win32 外壳）')
        return 0
    if not os.path.isdir(GAME[0]):
        print('SKIP: 缺游戏目录 %s' % GAME[0])
        return 0
    if not os.path.isdir(OUT):
        os.makedirs(OUT)

    shots = {k: os.path.join(OUT, 'openrender-%s.bmp' % k)
             for k in ('early', 'fade', 'done', 'static')}
    logs = {k: os.path.join(OUT, 'openrender-%s.log' % k)
            for k in ('early', 'fade', 'done', 'static')}
    plan = [
        ('early', ['--opening', '--shot-at', '250'], 0.9),
        ('fade', ['--opening', '--shot-at', '2900'], 3.4),
        ('done', ['--opening', '--shot-at', '8200'], 8.8),
        ('static', ['--shot-at', '1600'], 2.2),
    ]
    for key, extra, seconds in plan:
        for p in (shots[key], logs[key]):
            if os.path.isfile(p):
                os.remove(p)
        rc = run_shell(shots[key], logs[key], extra, seconds)
        check(rc == 0 and os.path.isfile(shots[key]),
              '%s: 外壳退出码 0 且写出截图（rc=%s）' % (key, rc))

    if fails:
        print('\nFAIL: 外壳没跑起来')
        return 1

    early = bmp_pixels(shots['early'])
    fade = bmp_pixels(shots['fade'])
    done = bmp_pixels(shots['done'])
    static = bmp_pixels(shots['static'])
    check(all(x is not None for x in (early, fade, done, static)),
          '四张 BMP 都读得出来')

    print('== 1. 开场早期是白底（白场）==')
    er, eg, eb, ewhite = stats(early)
    print('  early mean=(%.1f,%.1f,%.1f) near_white=%.3f' % (er, eg, eb, ewhite))
    check(ewhite > 0.90, '早期帧近白像素 > 90%%（实际 %.1f%%）' % (ewhite * 100))

    print('== 2. 白底淡出：早期 vs 淡出中必须明显不同 ==')
    e_f, e_ch = mean_abs(early, fade)
    print('  mean_abs=%.3f changed=%.2f%%' % (e_f, e_ch))
    check(e_f > 20.0, '白底淡出后 mean_abs > 20（实际 %.2f）' % e_f)

    print('== 3. 淡出中露出的是**标题背景**（脚本先建标题场景再跑开场）==')
    fr, fg, fb, fwhite = stats(fade)
    print('  fade  mean=(%.1f,%.1f,%.1f) near_white=%.3f' % (fr, fg, fb, fwhite))
    check(fb - fr > 30.0,
          '标题背景偏蓝：平均 B - R > 30（实际 %.1f）' % (fb - fr))
    check(fwhite < 0.80,
          '淡出中不再以白色为主（近白 %.1f%% < 80%%）' % (fwhite * 100))

    print('== 4. 开场结束 == 静态标题帧（逐字节）==')
    d_s, d_ch = mean_abs(done, static)
    print('  mean_abs=%.6f changed=%.4f%%' % (d_s, d_ch))
    check(d_s == 0.0, '开场结束帧与静态标题帧 mean_abs == 0（实际 %.6f）' % d_s)
    check(d_ch == 0.0, '没有差异像素（实际 %.4f%%）' % d_ch)

    print('== 5. 淡出中与静态标题帧不同（logo 还没消失）==')
    f_s, f_ch = mean_abs(fade, static)
    print('  mean_abs=%.3f changed=%.2f%%' % (f_s, f_ch))
    check(f_s > 20.0, '淡出中 vs 静态 mean_abs > 20（实际 %.2f）' % f_s)

    print('== 6. 日志 ==')
    for key in ('early', 'fade', 'done', 'static'):
        text = read_log(logs[key])
        if key == 'static':
            check('opening:' not in text, 'static 那次没有开场日志')
            continue
        check('6/6 layers loaded' in text,
              '%s: 六层素材都载入（6/6 layers loaded）' % key)
        if key == 'done':
            check(re.search(r'opening: finished in \d+ ms \(6 layers\)', text)
                  is not None,
                  '%s: 记录开场结束（opening: finished in ... 6 layers）' % key)
        else:
            check(re.search(r'opening: finished in', text) is None,
                  '%s: 抓完图就退出，开场还没结束（无 finished 日志）' % key)
    te = opening_t(read_log(logs['early']))
    tf = opening_t(read_log(logs['fade']))
    td = opening_t(read_log(logs['done']))
    print('  开场时刻：early=%s fade=%s done=%s' % (te, tf, td))
    check(te is not None and 100 <= te <= 900,
          'early 落在开场前 0.9 s 内（%s ms）' % te)
    check(tf is not None and 2400 <= tf <= 3400,
          'fade 落在淡出中段（%s ms）' % tf)
    check(td is None, 'done 那次开场已经结束（日志里没有 opening t=）')
    done_log = read_log(logs['done'])
    i_fin = done_log.find('opening: finished in')
    i_fade = done_log.find('title: script entrance started')
    check(i_fin >= 0 and i_fade > i_fin,
          '标题逐元素进场在开场结束之后才开始（finished@%d < entrance@%d）'
          % (i_fin, i_fade))

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 开场从白场开始、淡出露出标题背景、结束时与静态标题帧逐字节相同')
    return 0


if __name__ == '__main__':
    sys.exit(main())