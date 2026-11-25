#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""标题背景缩放的**端到端**门禁（星空）：脚本 MotionMoveS2 2000→1000 / 40000 ms。

单元测试 `tests/test_titlebg.c` 证明时间轴数学；`tests/test_gfx_xform.c` §9 证明
`gfx_draw_image_cropped_scaled` 的裁剪+缩放语义；这里把外壳焊上：

  A. 星空 `--title-bg-anim --title-anim --shot-at 2200`  → 背景放大中（约 1.9×）
  B. 星空 `--title-bg-anim --title-anim --shot-at 41200` → 40 s 动画结束 + 进场结束
  C. 星空静态标题（不开动画）                             → 基线口径
  D. 五彩斑斓 `--title-bg-anim` → 四层 path A/B 动画（见 4b 段）

断言：
  1. **A != C**（mean_abs > 10、差异像素 > 10%）：背景缩放真的在画面上生效；
  2. **B == C 逐字节相同**：缩放到 1000‰ 后走恒等路径，与静态帧一致
     （同时证明缓存/量化不会污染终态）；
  3. **A != B**；
  4. 日志：A/B 有 `bg zoom started (hoshimemo`；D 有 `iroseka: not implemented, static`；
  5. 平滑度：`--shot-every 60` 抓 4 s 序列，相邻帧差 min > 0.02（没有
     "卡住不动"）、max < 3·median+1（没有跳变）；
  6. 性能：A 那次的外壳 fps ≥ 45（逐帧双线性重算，实测 ~50-59）；
  7. **五彩斑斓四层**（path A/B）：动画中 vs 静态 mean_abs > 10、fps ≥ 35
     （半分辨率合成）、日志写明四层/path A/B、相邻帧差同样没有平台/跳变。

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
            '--title-no-hover',
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
             for k in ('mid', 'done', 'static', 'iro', 'iro_static')}
    logs = {k: os.path.join(OUT, 'bgzoom-%s.log' % k)
            for k in ('mid', 'done', 'static', 'iro', 'iro_static')}
    plan = [
        ('mid', HOSHI, ['--title-bg-anim', '--title-anim', '--shot-at', '2200'],
         2.9),
        ('done', HOSHI, ['--title-bg-anim', '--title-anim', '--shot-at',
                         '41200'], 41.8),
        ('static', HOSHI, ['--shot-at', '1600'], 2.2),
        ('iro', IRO, ['--title-bg-anim', '--title-anim', '--shot-at', '3000'],
         3.8),
        ('iro_static', IRO, ['--shot-at', '1600'], 2.2),
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
    check('not implemented' not in iro_log,
          '五彩斑斓：背景动画已实现（不再打印 not implemented）')
    check('bg zoom started (hoshimemo' not in iro_log,
          '五彩斑斓没有跑星空的时间轴')
    check('bg zoom started (iroseka 4 layers' in iro_log,
          '五彩斑斓跑自己的四层时间轴')

    print('== 4b. 五彩斑斓：四层 path A/B 背景动画 ==')
    iro_mid = bmp_pixels(shots['iro'])
    iro_static = bmp_pixels(shots['iro_static'])
    check(iro_mid is not None and iro_static is not None,
          '五彩两张 BMP 都读得出来')
    iro_log = read_log(logs['iro'])
    check('bg zoom started (iroseka 4 layers' in iro_log,
          '日志打印 iroseka 四层路径')
    check('path A/B' in iro_log, '日志注明 path A/B')
    if iro_mid is not None and iro_static is not None:
        m_s, p_s = mean_abs(iro_mid, iro_static)
        print('  iroseka mid vs static mean_abs=%.3f changed=%.2f%%'
              % (m_s, p_s))
        check(m_s > 10.0,
              '五彩：动画中 vs 静态 mean_abs > 10（实际 %.2f）' % m_s)
        check(p_s > 10.0, '五彩：差异像素 > 10%%（实际 %.2f%%）' % p_s)
        m = re.search(r'\(([0-9.]+) fps\)', iro_log)
        iro_fps = int(float(m.group(1))) if m else -1
        print('  iroseka fps=%s' % iro_fps)
        check(iro_fps >= 35,
              '五彩：四层合成时 fps ≥ 35（实际 %s；半分辨率合成）' % iro_fps)

    print('== 5. 平滑度：抓 4 s 帧序列，相邻帧差不能有平台/尖峰 ==')
    seq_dir = os.path.join(OUT, 'bgzseq')
    os.makedirs(seq_dir, exist_ok=True)
    for f in os.listdir(seq_dir):
        if f.endswith('.bmp'):
            os.remove(os.path.join(seq_dir, f))
    seq_log = os.path.join(OUT, 'bgzseq-gate.log')
    rc = run_shell(HOSHI, '', seq_log,
                   ['--title-bg-anim', '--title-anim', '--shot-every', '60',
                    '--shot-div', '4', '--shot-prefix',
                    os.path.join(seq_dir, 'g')], 4.6)
    check(rc == 0, '抓帧序列外壳退出码 0（实际 %s）' % rc)
    names = sorted(f for f in os.listdir(seq_dir) if f.endswith('.bmp'))
    check(len(names) >= 30, '抓到足够多的帧（%d 张，>=30）' % len(names))
    frames = [bmp_pixels(os.path.join(seq_dir, f)) for f in names]
    frames = [f for f in frames if f is not None]
    check(len(frames) >= 30, '帧都读得出来（%d）' % len(frames))
    diffs = []
    for a, b in zip(frames, frames[1:]):
        d, _ = mean_abs(a, b)
        diffs.append(d)
    if diffs:
        ds = sorted(diffs)
        med = ds[len(ds) // 2]
        mn = ds[0]
        mx = ds[-1]
        print('  相邻帧差: min=%.4f median=%.4f max=%.4f' % (mn, med, mx))
        check(mn > 0.02, '没有"卡住不动"的相邻帧（min %.4f > 0.02）' % mn)
        check(med > 0.1, '相邻帧差中位数 > 0.1（%.4f，动画在连续推进）' % med)
        check(mx < med * 3.0 + 1.0,
              '没有突兀跳变（max %.4f < 3*median+1）' % mx)
    else:
        check(False, '相邻帧差算不出来')

    print('== 5b. 五彩斑斓的平滑度序列 ==')
    iro_seq = os.path.join(OUT, 'bgzseq-iro')
    os.makedirs(iro_seq, exist_ok=True)
    for f in os.listdir(iro_seq):
        if f.endswith('.bmp'):
            os.remove(os.path.join(iro_seq, f))
    iro_seq_log = os.path.join(OUT, 'bgzseq-iro.log')
    rc = run_shell(IRO, '', iro_seq_log,
                   ['--title-bg-anim', '--title-anim', '--shot-every', '60',
                    '--shot-div', '4', '--shot-prefix',
                    os.path.join(iro_seq, 'g')], 4.6)
    check(rc == 0, '五彩抓帧序列外壳退出码 0（实际 %s）' % rc)
    inames = sorted(f for f in os.listdir(iro_seq) if f.endswith('.bmp'))
    check(len(inames) >= 30, '五彩抓到足够多的帧（%d 张，>=30）'
          % len(inames))
    iframes = [bmp_pixels(os.path.join(iro_seq, f)) for f in inames]
    iframes = [f for f in iframes if f is not None]
    idiffs = []
    for a, b in zip(iframes, iframes[1:]):
        d, _ = mean_abs(a, b)
        idiffs.append(d)
    if idiffs:
        ids = sorted(idiffs)
        imed = ids[len(ids) // 2]
        print('  五彩相邻帧差: min=%.4f median=%.4f max=%.4f'
              % (ids[0], imed, ids[-1]))
        check(ids[0] > 0.01,
              '五彩：没有卡住的相邻帧（min %.4f > 0.01）' % ids[0])
        check(ids[-1] < imed * 3.0 + 1.0,
              '五彩：没有突兀跳变（max %.4f）' % ids[-1])
    else:
        check(False, '五彩相邻帧差算不出来')

    print('== 6. 性能（≥45 fps；带缓存的逐帧重算）==')
    m = re.search(r'\(([0-9.]+) fps\)', mid_log)
    fps = int(float(m.group(1))) if m else -1
    print('  mid run fps=%s' % fps)
    check(fps >= 45, '缩放期间外壳 fps ≥ 45（实际 %s）' % fps)

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 星空 2000→1000/40 s（~%d fps，结束与静态逐字节相同）；'
          '五彩四层 path A/B 缩放+旋转（~%d fps，半分辨率合成，'
          '动画中与静态 mean_abs %.2f）' % (fps, iro_fps, m_s))
    return 0


if __name__ == '__main__':
    sys.exit(main())