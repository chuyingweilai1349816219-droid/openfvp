#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""读档续玩 step-1 的端到端门禁：`--load-restart` 读槽文件 + 复位场景 + 重启脚本。

结构分析依据（`ref/exe-load-flow.json`）：Load(40) 只置请求；
exe 的消费路径是 读槽文件 → 场景/图元树复位(0x438D20) → 脚本引擎复位(0x441350)
→ 线程 +0xFD9 从入口重跑，全局变量（save.bin）保持已灌入的值。

本站 step-1 实现（`--load-restart`）：
1. 读槽文件并解析（章节名/对话行长度、缩略图尺寸、尾段长度）；
2. 复位场景：prim/motion/文本窗口全清；
3. 重建 VM 并从入口重启，保留 globals / globals_set / flags；
4. 槽尾"引擎快照"里**台词历史环**（第 20 段）已经回灌（读档 step-2 第一段，
   src/savehist.c；格式与读档路径 0x43F25A 已从静态分析冻结）；
   其余段还没逆清，**不应用**（文档写明）。

断言：
  A. `--load-restart --debug-load-request 485`：日志有槽文件解析
     （thumb=200x113、tail 长度）、复位、`restarted at entry pc=0x...`；
     重启后脚本继续跑（总步数 > 30000、至少 1 行文本）、退出码 0、
     结束帧有内容（采样非零像素 > 20；旧的“平均亮度 > 5”是合成对话框的副产物，
     2026-11-26 文本改图元驱动后不再适用）；
  B. 不带 `--load-restart`：日志明确 `not implemented` 且提示
     `pass --load-restart to restart`（行为未被悄悄改变）；
  C. 历史环回灌：槽 485 → `history: restored count=1 pos=0`；
     槽 905 → `count=11 pos=10 filled=11`，最新台词是"谁这样说道。"
     （槽尾历史环在 6 份真实存档里的解析见 verify_exe_history.py）。

缺 bin/fvp.exe 或游戏目录/槽文件时 SKIP。

用法：python tests/verify_load_restart.py
"""
import io
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
GAME = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD')
SLOT = 485

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def run(extra, log, shot, seconds):
    args = [EXE, '--no-input', '--game', GAME, '--script', 'iroseka.hcb',
            '--no-audio', '--scale', '1', '--run-seconds', '%g' % seconds,
            '--screenshot', shot, '--log', log] + extra
    try:
        return subprocess.run(args, timeout=180).returncode
    except subprocess.TimeoutExpired:
        return 1


def read_log(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as fp:
            return fp.read()
    except OSError:
        return ''


def main():
    if not os.path.isfile(EXE):
        print('SKIP: bin/fvp.exe 不存在')
        return 0
    slot_path = os.path.join(GAME, 'save', 's%03d.bin' % SLOT)
    if not os.path.isfile(slot_path):
        print('SKIP: 缺少原版槽文件 %s' % slot_path)
        return 0
    if not os.path.isdir(OUT):
        os.makedirs(OUT)

    print('== A. --load-restart：读槽 + 复位 + 重启 ==')
    shot = os.path.join(OUT, 'loadrestart-a.bmp')
    log = os.path.join(OUT, 'loadrestart-a.log')
    for p in (shot, log):
        if os.path.isfile(p):
            os.remove(p)
    rc = run(['--load-restart', '--debug-load-request', str(SLOT)], log, shot,
             5.0)
    text = read_log(log)
    check(rc == 0 and os.path.isfile(shot), '外壳退出码 0 且写出截图')
    m = re.search(r'load: slot %d file .*?\((\d+) bytes\) -> parsed'
                  % SLOT, text)
    check(m is not None, '日志有槽文件解析（slot %d, parsed）' % SLOT)
    if m:
        check(int(m.group(1)) > 100000, '槽文件大小合理（%s 字节）' % m.group(1))
    check('thumb=200x113' in text, '缩略图尺寸 200x113（来自 save.bin 头）')
    check('tail=' in text, '日志带尾段长度（引擎快照）')
    check('scene reset (prims/motion/text)' in text, '场景复位（prim/motion/text）')
    check(re.search(r'restarted at entry pc=0x[0-9a-f]+', text) is not None,
          '脚本引擎从入口重启（日志带 pc）')
    check('history ring scan -> found' in text,
          '槽尾台词历史环定位成功（读档 step-2 第一段）')
    check(re.search(r'history: restored count=1 pos=0 filled=1', text) is not None,
          '槽 485 的历史回灌 count=1 pos=0')
    ms = re.findall(r'vm\s+(\d+) steps total', text)
    total_steps = int(ms[-1]) if ms else 0
    check(total_steps > 30000, '重启后脚本继续执行（总步数 %d > 30000）'
          % total_steps)
    check('text lines' in text and ' 0 text lines' not in text,
          '重启后至少产生 1 行文本')
    px = bmp_pixels(shot)
    if px is not None:
        n = 0
        s = 0
        ink = 0
        for row in px[::8]:
            for p in row[::8]:
                s += p[0] + p[1] + p[2]
                n += 1
                if p[0] or p[1] or p[2]:
                    ink += 1
        mean = s / (3.0 * n) if n else 0.0
        print('  结束帧 mean=%.2f ink=%d/%d' % (mean, ink, n))
        # 2026-11-26：默认文本渲染已改成图元驱动，引擎不再画合成对话框，
        # 结束帧的“整体平均亮度”不再是有效判据（黑底画面 mean 会 <1）。
        # 活性判据改成“采样里确有非零像素”（真的画了东西）。
        check(ink > 20, '结束帧有内容（ink %d > 20）' % ink)
    else:
        check(False, '截图读不出来')

    print('== C. 历史环回灌（槽 905：11 条台词）==')
    slot905 = os.path.join(GAME, 'save', 's905.bin')
    if not os.path.isfile(slot905):
        print('  SKIP: 缺少 s905.bin')
    else:
        shot_c = os.path.join(OUT, 'loadrestart-c.bmp')
        log_c = os.path.join(OUT, 'loadrestart-c.log')
        for p in (shot_c, log_c):
            if os.path.isfile(p):
                os.remove(p)
        rc_c = run(['--load-restart', '--debug-load-request', '905'],
                   log_c, shot_c, 4.0)
        text_c = read_log(log_c)
        check(rc_c == 0, '外壳退出码 0')
        check('history ring scan -> found' in text_c,
              '槽 905 的历史环定位成功')
        m_c = re.search(r'history: restored count=(\d+) pos=(\d+) filled=(\d+)',
                        text_c)
        check(m_c is not None, '日志有 history: restored 行')
        if m_c:
            check((int(m_c.group(1)), int(m_c.group(2)), int(m_c.group(3))) ==
                  (11, 10, 11),
                  '历史回灌 count=11 pos=10 filled=11（实际 %s/%s/%s）'
                  % (m_c.group(1), m_c.group(2), m_c.group(3)))
        check('newest="　谁这样说道。"' in text_c,
              '最新一条台词是存档里的中文台词（编码自动判定）')

        print('== D. 每槽全局变量（第 6 段）解码与回灌 ==')


    def parse_globals(text):
        m = re.search(r'load: slot globals -> (\d+) cells \(types 0:(\d+) '
                      r'1:(\d+) 2:(\d+) 3:(\d+) 4:(\d+) 5:(\d+) 6:(\d+)\)',
                      text)
        if not m:
            return None
        n = int(m.group(1))
        counts = [int(m.group(i)) for i in range(2, 9)]
        a = re.search(r'load: globals applied (\d+) int cell\(s\), kept (\d+)', text)
        applied = int(a.group(1)) if a else -1
        kept = int(a.group(2)) if a else -1
        return n, counts, applied, kept

    for slot, want_t5 in ((485, 1), (905, 2)):
        log = os.path.join(OUT, 'lr-globals-%d.log' % slot)
        shot = os.path.join(OUT, 'lr-globals-%d.bmp' % slot)
        rc = run(['--load-restart', '--debug-load-request', str(slot)], log,
                 shot, 3)
        text = read_log(log)
        g = parse_globals(text)
        check(rc == 0 and g is not None,
              '槽 %d：日志有槽全局变量解码行' % slot)
        if g:
            n, counts, applied, kept = g
            check(n == 1549, '槽 %d：解码 1549 条（实际 %d）' % (slot, n))
            check(sum(counts) == n, '槽 %d：各类型计数之和 = %d（实际 %d）'
                  % (slot, n, sum(counts)))
            check(counts[5] == want_t5,
                  '槽 %d：type5（动态字符串）%d 条，与不变量分布一致'
                  % (slot, counts[5]))
            check(applied == counts[2] and applied + kept == n,
                  '槽 %d：只回灌 type2=int（applied=%d == type2=%d，applied+kept=%d）'
                  % (slot, applied, counts[2], applied + kept))
            check('only type 2 = int is mapped so far' in text,
                  '槽 %d：日志写明当前只映射 type 2' % slot)

    print('== B. 不带 --load-restart：旧行为（只记日志）==')
    shot_b = os.path.join(OUT, 'loadrestart-b.bmp')
    log_b = os.path.join(OUT, 'loadrestart-b.log')
    for p in (shot_b, log_b):
        if os.path.isfile(p):
            os.remove(p)
    rc_b = run(['--debug-load-request', str(SLOT)], log_b, shot_b, 3.0)
    text_b = read_log(log_b)
    check(rc_b == 0, '外壳退出码 0')
    check('not implemented yet' in text_b, '日志写明快照恢复未实现')
    check('pass --load-restart to restart' in text_b,
          '日志提示 --load-restart 开关')
    check('scene reset' not in text_b, '没开开关时不做场景复位')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: --load-restart 读槽文件 → 场景复位 → 脚本引擎从入口重启'
          '（globals/flags 保留），重启后脚本继续跑且画面非黑')
    return 0


if __name__ == '__main__':
    sys.exit(main())