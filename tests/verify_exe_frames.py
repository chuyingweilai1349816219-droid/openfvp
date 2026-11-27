#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""帧/栈模型 A/B 门禁：`bin/fvm.exe` 的回归口径 vs `--exe-frames`（按 exe 字节）。

为什么单独一道门禁（2026-11-27 第 327 轮）：

* exe 的返回例程 `0x444660` 在 `0x44473D` 明算 `sp = 标记槽 - A`
  （`0x824` 先 `dec`，再 `sub eax,edx`，**不减 B**）；
  `ref/fvm_ref.py` 是"局部数组 + 实参数组 + call 时清栈"的另一套模型。
* 两套口径在正文模板 `0x3A11F` 上不等价：旧口径每次从"有局部变量"的函数
  返回都让 sp 少 B 格，`pushstack 251`（= base-5 = arg1）因此读到过期槽。
* 冻结文本基线 `ref/ref-900k.txt` 是旧口径产出的，所以 `fvm` 默认旧口径、
  游戏外壳默认按 exe 走。这道门禁钉住"两种口径都还在、且差异是已知的那一处"。

断言（每一条都能复现）：
  1. `fvm` 默认 900k 步的输出与 `ref/ref-900k.txt` 逐字节相同（= compare.py 的结论）。
  2. `fvm --exe-frames` 900k 步能走出正文（行数 > 0），且与冻结基线不同。
  3. 两种口径的第一处差异落在正文文本上（不是"整段消失"）。
"""
import io
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.abspath(os.path.join(HERE, '..'))
EXE = os.path.join(FVP, 'bin', 'fvm.exe')
REF = os.path.join(FVP, 'ref', 'ref-900k.txt')
SCRIPT = r'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\iroseka.hcb'
STEPS = '900000'

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def run(out, extra):
    cmd = [EXE, SCRIPT, '--steps', STEPS, '--click', '600', '--enc', 'cp932',
           '--quiet', '--text-out', out] + extra
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        print(proc.stderr.decode('utf-8', 'replace'))
    return proc.returncode


def main():
    if not os.path.isfile(EXE):
        print('SKIP: 缺 %s（先 mingw32-make -f Makefile all）' % EXE)
        return 0
    if not os.path.isfile(SCRIPT):
        print('SKIP: 缺脚本 %s' % SCRIPT)
        return 0
    out_dir = os.path.join(FVP, 'out', 'c')
    os.makedirs(out_dir, exist_ok=True)

    legacy = os.path.join(out_dir, 'frames-legacy.txt')
    exe = os.path.join(out_dir, 'frames-exe.txt')

    print('== 1. 回归口径（fvm 默认）==')
    rc = run(legacy, [])
    check(rc == 0, 'fvm 退出码 0（实际 %d）' % rc)
    ref = io.open(REF, 'rb').read()
    got = io.open(legacy, 'rb').read() if os.path.isfile(legacy) else b''
    check(got == ref, '与 ref/ref-900k.txt 逐字节相同（%d vs %d 字节）'
          % (len(got), len(ref)))

    print('== 2. exe 口径（--exe-frames）==')
    rc = run(exe, ['--exe-frames'])
    check(rc == 0, 'fvm --exe-frames 退出码 0（实际 %d）' % rc)
    data = io.open(exe, 'rb').read() if os.path.isfile(exe) else b''
    n_lines = data.count(b'\n')
    check(n_lines > 0, 'exe 口径走出正文（%d 行）' % n_lines)
    check(data != ref, 'exe 口径与冻结基线不同（这正是两套模型的已知差异）')
    check(len(data) < len(ref),
          'exe 口径更短（%d < %d 字节：冗余/重复行被消掉，不是整段消失）'
          % (len(data), len(ref)))

    print('== 3. 差异落在正文文本上 ==')
    a = ref.split(b'\r\n')
    b = data.split(b'\r\n')
    n = min(len(a), len(b))
    first = next((i for i in range(n) if a[i] != b[i]), -1)
    check(first > 0, '第一处差异在第 %d 行（不是第 1 行）' % (first + 1))
    if first > 0:
        check(a[first].startswith(b'[0]') or a[first].startswith(b'[9]'),
              '第一处差异是文本行：%r' % a[first][:40])

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 两套帧/栈模型都还在 —— fvm 默认复现冻结基线，'
          '--exe-frames 按 exe 字节（0x44473D）走')
    return 0


if __name__ == '__main__':
    sys.exit(main())