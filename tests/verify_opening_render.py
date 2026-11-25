#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""开场 FAVORITE logo 的**端到端**门禁（两个作品）：真的启动外壳，按脚本时间轴抓帧。

单元测试 `tests/test_openseq.c` 证明时间轴数学；门禁
`tests/verify_opening_anim.py` 证明 C 里的常量 == 脚本（两个作品）；这里把外壳焊上：

  A. `--opening --shot-at 早期`   开场早期（白底 + logo 淡入中）
  B. `--opening --shot-at 2900`   淡出中（白底快没了，标题背景已经露出来）
  C. `--opening --shot-at 结束`   开场 3800 + 标题进场跑完之后
  D. 不开 `--opening` 的静态标题（基线口径）

注意两作的标题进场时长不同（五彩 4000 ms、星空 3000 ms），所以 C 的时刻不同。

断言（每个作品）：
  1. **A 是白底**（近白像素 > 90%）；
  2. **A != B**：白底淡出确实发生了；
  3. **B 已经能看见标题背景**（蓝分量高于红分量、且不再以白色为主）：脚本先建标题
     场景（0x590CC call 0x584D8）再跑开场（0x591EC），白底淡出露出的是标题背景；
  4. **C == D 逐字节相同**：开场六层 + 标题进场都归位后 == 静态标题帧；
  5. 日志：A/B 有 `6/6 layers loaded` 与 `opening t=`；C 有 `opening: finished in ... 6 layers`
     且没有 `opening t=`；`title: script entrance started` 在 finished 之后。

缺 `bin/fvp.exe` 或游戏目录时 SKIP 并返回 0。

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
# done：开场 3800 ms + 标题进场（五彩 4000 / 星空 3000）+ 启动/解码余量；
# 余量给到 ~1.5 s，避免首帧慢时"进场还差几十毫秒"造成非逐字节相等
GAMES = {
    'iroseka': {
        'dir': r'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD',
        'script': 'iroseka.hcb',
        'early': 250, 'done': 9400, 'run': 10.0,
    },
    'hoshimemo': {
        'dir': r'C:\Users\Mechrevo\Downloads\hoshimemo_installed',
        'script': 'Hoshimemo_HD.hcb',
        'early': 700, 'done': 9000, 'run': 9.6,
    },
}

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def run_shell(g, shot, log, extra, seconds):
    args = [EXE, '--no-input', '--game', g['dir'], '--script', g['script'],
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


def check_game(name, g):
    print('==== %s ==' % name)
    shots = {k: os.path.join(OUT, 'openrender-%s-%s.bmp' % (name, k))
             for k in ('early', 'fade', 'done', 'static')}
    logs = {k: os.path.join(OUT, 'openrender-%s-%s.log' % (name, k))
            for k in ('early', 'fade', 'done', 'static')}
    plan = [
        ('early', ['--opening', '--shot-at', str(g['early'])],
         g['early'] / 1000.0 + 0.7),
        ('fade', ['--opening', '--shot-at', '2900'], 3.4),
        ('done', ['--opening', '--shot-at', str(g['done'])], g['run']),
        ('static', ['--shot-at', '1600'], 2.2),
    ]
    for key, extra, seconds in plan:
        for pth in (shots[key], logs[key]):
            if os.path.isfile(pth):
                os.remove(pth)
        rc = run_shell(g, shots[key], logs[key], extra, seconds)
        check(rc == 0 and os.path.isfile(shots[key]),
              '%s/%s: 外壳退出码 0 且写出截图（rc=%s）' % (name, key, rc))
    if fails:
        return None

    early = bmp_pixels(shots['early'])
    fade = bmp_pixels(shots['fade'])
    done = bmp_pixels(shots['done'])
    static = bmp_pixels(shots['static'])
    if not check(all(x is not None for x in (early, fade, done, static)),
                 '%s: 四张 BMP 都读得出来' % name):
        return None

    print('-- 1. 开场早期是白底 --')
    er, eg, eb, ewhite = stats(early)
    print('  %s early mean=(%.1f,%.1f,%.1f) near_white=%.3f'
          % (name, er, eg, eb, ewhite))
    check(ewhite > 0.90, '%s: 早期帧近白像素 > 90%%（%.1f%%）'
          % (name, ewhite * 100))

    print('-- 2. 白底淡出 --')
    e_f, _ = mean_abs(early, fade)
    print('  %s early vs fade mean_abs=%.3f' % (name, e_f))
    check(e_f > 20.0, '%s: 白底淡出后 mean_abs > 20（%.2f）' % (name, e_f))

    print('-- 3. 淡出中露出标题背景 --')
    fr, fg, fb, fwhite = stats(fade)
    print('  %s fade mean=(%.1f,%.1f,%.1f) near_white=%.3f'
          % (name, fr, fg, fb, fwhite))
    check(fb > fr + 5.0, '%s: 标题背景蓝分量 > 红分量（B-R=%.1f）'
          % (name, fb - fr))
    check(fwhite < 0.80, '%s: 淡出中不再以白色为主（近白 %.1f%%）'
          % (name, fwhite * 100))

    print('-- 4. 结束帧 == 静态标题帧（逐字节）--')
    d_s, d_ch = mean_abs(done, static)
    print('  %s done vs static mean_abs=%.6f changed=%.4f%%'
          % (name, d_s, d_ch))
    check(d_s == 0.0, '%s: 结束帧 mean_abs == 0（%.6f）' % (name, d_s))
    check(d_ch == 0.0, '%s: 没有差异像素（%.4f%%）' % (name, d_ch))

    print('-- 5. 淡出中 != 静态标题 --')
    f_s, _ = mean_abs(fade, static)
    check(f_s > 20.0, '%s: 淡出中 vs 静态 mean_abs > 20（%.2f）' % (name, f_s))

    print('-- 6. 日志 --')
    for key in ('early', 'fade', 'done', 'static'):
        text = read_log(logs[key])
        if key == 'static':
            check('opening:' not in text, '%s/static 没有开场日志' % name)
            continue
        check('6/6 layers loaded' in text,
              '%s/%s 六层素材都载入（6/6）' % (name, key))
        if key == 'done':
            check(re.search(r'opening: finished in \d+ ms \(6 layers\)', text)
                  is not None, '%s/done 记录开场结束' % name)
        else:
            check('opening: finished in' not in text,
                  '%s/%s 抓完图就退出，开场还没结束' % (name, key))
    te = opening_t(read_log(logs['early']))
    tf = opening_t(read_log(logs['fade']))
    td = opening_t(read_log(logs['done']))
    print('  开场时刻：early=%s fade=%s done=%s' % (te, tf, td))
    check(te is not None and 100 <= te <= 1200,
          '%s: early 落在开场前 1.2 s 内（%s ms）' % (name, te))
    check(tf is not None and 2400 <= tf <= 3400,
          '%s: fade 落在淡出中段（%s ms）' % (name, tf))
    check(td is None, '%s: done 那次开场已经结束（无 opening t=）' % name)
    done_log = read_log(logs['done'])
    i_fin = done_log.find('opening: finished in')
    i_fade = done_log.find('title: script entrance started')
    check(i_fin >= 0 and i_fade > i_fin,
          '%s: 标题逐元素进场在开场结束之后才开始（%d < %d）'
          % (name, i_fin, i_fade))
    return e_f


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
    for name in have:
        check_game(name, GAMES[name])
    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 两个作品的开场都从白场开始、淡出露出标题背景，'
          '开场+标题进场结束后与静态标题帧逐字节相同')
    return 0


if __name__ == '__main__':
    sys.exit(main())