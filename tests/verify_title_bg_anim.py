#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""标题背景缩放的**端到端**门禁（星空）：脚本 MotionMoveS2 2000→1000 / 40000 ms。

单元测试 `tests/test_titlebg.c` 证明时间轴数学；`tests/test_gfx_xform.c` §9 证明
`gfx_draw_image_cropped_scaled` 的裁剪+缩放语义；这里把外壳焊上：

  A. 星空 `--title-bg-anim --title-anim --shot-at 2200`  → 背景放大中（约 1.9×）
  B. 星空 `--title-bg-anim --title-anim --shot-at 41200` → 40 s 动画结束 + 进场结束
  C. 星空静态标题（不开动画）                             → 基线口径
  D. 五彩斑斓 `--title-bg-anim` → 该作背景动画**尚未实现**，日志必须写明 static

断言：
  1. **A != C**（mean_abs > 10、差异像素 > 10%）：背景缩放真的在画面上生效；
  2. **B == C 逐字节相同**：缩放到 1000‰ 后走恒等路径，与静态帧一致
     （同时证明缓存/量化不会污染终态）；
  3. **A != B**；
  4. 日志：A/B 有 `bg zoom started (hoshimemo`；D 有 `iroseka: not implemented, static`；
  5. 性能：A 那次的外壳 fps ≥ 25（缓存生效；未缓存时实测 ~10 fps）。

缺 `bin/fvp.exe` 或游戏目录时 SKIP 并返回 0。

用法：python tests/verify_title_bg_anim.py
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
HOSHI = (r'C:\Users\Mechrevo\Downloads\hoshimemo_installed', 'Hoshimemo_HD.hcb')
IRO = (r'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD', 'iroseka.hcb')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def run_shell(game, shot, log, extra, seconds):
    args = [EXE, '--no-input', '--game', game[0], '--script', game[1],
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


def main():
    if not os.path.isfile(EXE):
        print('SKIP: bin/fvp.exe 不存在（未构建 win32 外壳）')
        return 0
    if not (os.path.isdir(HOSHI[0]) and os.path.isfile(
            os.path.join(HOSHI[0], HOSHI[1]))):
        print('SKIP: 缺星空游戏数据 %s' % HOSHI[0])
        return 0
    if not os.path.isdir(OUT):
        os.makedirs(OUT)

    shots = {k: os.path.join(OUT, 'bgzoom-%s.bmp' % k)
             for k in ('mid', 'done', 'static', 'iro')}
    logs = {k: os.path.join(OUT, 'bgzoom-%s.log' % k)
            for k in ('mid', 'done', 'static', 'iro')}
    plan = [
        ('mid', HOSHI, ['--title-bg-anim', '--title-anim', '--shot-at', '2200'],
         2.9),
        ('done', HOSHI, ['--title-bg-anim', '--title-anim', '--shot-at',
                         '41200'], 41.8),
        ('static', HOSHI, ['--shot-at', '1600'], 2.2),
        ('iro', IRO, ['--title-bg-anim', '--title-anim', '--shot-at', '2200'],
         2.9),
    ]
    for key, game, extra, seconds in plan:
        for p in (shots[key], logs[key]):
            if os.path.isfile(p):
                os.remove(p)
        rc = run_shell(game, shots[key], logs[key], extra, seconds)
        check(rc == 0 and os.path.isfile(shots[key]),
              '%s: 外壳退出码 0 且写出截图（rc=%s）' % (key, rc))
    if fails:
        print('\nFAIL: 外壳没跑起来')
        return 1

    mid = bmp_pixels(shots['mid'])
    done = bmp_pixels(shots['done'])
    static = bmp_pixels(shots['static'])
    check(all(x is not None for x in (mid, done, static)),
          '三张 BMP 都读得出来')

    print('== 1. 缩放中途 vs 静态：背景确实被放大 ==')
    m_s, p_s = mean_abs(mid, static)
    print('  mean_abs=%.3f changed=%.2f%%' % (m_s, p_s))
    check(m_s > 10.0, '中途 vs 静态 mean_abs > 10（实际 %.2f）' % m_s)
    check(p_s > 10.0, '差异像素 > 10%%（实际 %.2f%%）' % p_s)

    print('== 2. 动画结束 == 静态（1000‰ 走恒等路径）==')
    d_s, d_ch = mean_abs(done, static)
    print('  done vs static mean_abs=%.6f changed=%.4f%%' % (d_s, d_ch))
    check(d_s == 0.0, '结束帧 mean_abs == 0（实际 %.6f）' % d_s)
    check(d_ch == 0.0, '没有差异像素（实际 %.4f%%）' % d_ch)

    print('== 3. 中途 != 结束 ==')
    m_d, _ = mean_abs(mid, done)
    check(m_d > 10.0, '中途 vs 结束 mean_abs > 10（实际 %.2f）' % m_d)

    print('== 4. 日志 ==')
    mid_log = read_log(logs['mid'])
    done_log = read_log(logs['done'])
    check('bg zoom started (hoshimemo' in mid_log,
          'mid 打印 bg zoom started (hoshimemo ...)')
    check('bg zoom started (hoshimemo' in done_log,
          'done 打印 bg zoom started (hoshimemo ...)')
    iro_log = read_log(logs['iro'])
    check('iroseka: not implemented, static' in iro_log,
          '五彩斑斓：--title-bg-anim 明确记为"尚未实现、static"')
    check('bg zoom started (hoshimemo' not in iro_log,
          '五彩斑斓没有跑星空的时间轴')

    print('== 5. 性能：缓存生效（≥25 fps；未缓存时实测 ~10 fps）==')
    m = re.search(r'\(([0-9.]+) fps\)', mid_log)
    fps = int(float(m.group(1))) if m else -1
    print('  mid run fps=%s' % fps)
    check(fps >= 25, '缩放期间外壳 fps ≥ 25（实际 %s）' % fps)

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 星空标题背景按脚本 2000→1000/40 s 缩放（缓存后 ~%d fps），'
          '动画结束与静态帧逐字节相同；五彩斑斓明确记为未实现' % fps)
    return 0


if __name__ == '__main__':
    sys.exit(main())