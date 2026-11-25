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
4. 槽尾"引擎快照"里还没逆清的段**不应用**（文档写明）。

断言：
  A. `--load-restart --debug-load-request 485`：日志有槽文件解析
     （thumb=200x113、tail 长度）、复位、`restarted at entry pc=0x...`；
     重启后脚本继续跑（总步数 > 30000、至少 1 行文本）、退出码 0、
     结束帧非黑（mean > 5）；
  B. 不带 `--load-restart`：日志明确 `not implemented` 且提示
     `pass --load-restart to restart`（行为未被悄悄改变）。

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
        for row in px[::8]:
            for p in row[::8]:
                s += p[0] + p[1] + p[2]
                n += 1
        mean = s / (3.0 * n) if n else 0.0
        print('  结束帧 mean=%.2f' % mean)
        check(mean > 5.0, '结束帧非黑（mean %.2f > 5）' % mean)
    else:
        check(False, '截图读不出来')

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