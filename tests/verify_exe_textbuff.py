#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""TextBuff 尺寸上限/越界行为的门禁（A 类）。

`scripts/extract-exe-textbuff.py` 把 `0x430EA0`（TextBuff 的构造/改尺寸函数）的
上限检查与越界分支抄成 `ref/exe-textbuff.json`。本门禁断言：

  1. 重新提取 == 基线（逐字节）；
  2. 上限 w/h 各 0x400，越界走 `0x430FF8`；
  3. 越界分支是**抛异常**（`push $0x80` → 构造 → 格式化 → `call 0x44837C`），
     不是静默返回；
  4. 未越界时面板尺寸写 `tw+0x5DC/0x5DE`、缓冲 `w*h*4` 挂在 `tw+0x520`；
  5. C 侧一致性：`gfx_text_set_buff()` 用同一个 `0x400` 上限并在越界时返回 0
     （VM 没有异常语义，这是能做到的最近似行为）。

缺 `work/iroseka_HD.asm` 时 SKIP 并返回 0。

用法：python tests/verify_exe_textbuff.py
"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-textbuff.py')
REF = os.path.join(FVP, 'ref', 'exe-textbuff.json')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def read(path):
    return io.open(path, encoding='utf-8').read()


def main():
    asm = os.path.join(FVP, 'work', 'iroseka_HD.asm')
    if not os.path.isfile(asm):
        print('SKIP: 缺原版程序清单 %s（work/ 不入库）' % asm)
        return 0
    if not os.path.isfile(REF):
        print('FAIL: 缺基线 %s' % REF)
        return 1

    print('== 1. 重新提取并与基线逐字节比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', EXTRACT, '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=600)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0,
          'extract-exe-textbuff.py --check 退出码 0（实际 %d）' % proc.returncode)

    ref = json.loads(read(REF))
    lim = ref.get('limit') or {}
    ok = ref.get('on_ok') or {}

    print('== 2. 上限 ==')
    check(lim.get('w_max') == 0x400 and lim.get('h_max') == 0x400,
          'w/h 上限各 0x400（实际 %r/%r）' % (lim.get('w_max'), lim.get('h_max')))
    check(lim.get('check_pc') == ['0x430ea8', '0x430eba'],
          '两处检查指令（实际 %r）' % (lim.get('check_pc'),))

    print('== 3. 越界 = 抛异常（不是静默放弃）==')
    ov = lim.get('on_overflow') or ''
    for key in ('0x80', '0x447C05', '0x4258F0', '0x44837C'):
        check(key in ov or key.lower() in ov.lower(), '越界分支提到 %s' % key)

    print('== 4. 未越界时的写入 ==')
    check(ok.get('panel_w') == 'tw+0x5dc' and ok.get('panel_h') == 'tw+0x5de',
          '面板尺寸写 tw+0x5DC/0x5DE')
    check('w*h*4' in (ok.get('buffer') or ''), '缓冲 = w*h*4 挂在 tw+0x520')

    print('== 5. C 侧一致性 ==')
    gfx = read(os.path.join(FVP, 'src', 'gfx.c'))
    check('if (w > 0x400 || h > 0x400) {' in gfx,
          'gfx_text_set_buff() 用同一个 0x400 上限')
    seg = gfx[gfx.index('int gfx_text_set_buff'):]
    seg = seg[:seg.index('\n}')]
    check('return 0;' in seg, '越界时返回 0（VM 无异常语义，最接近的近似）')
    concl = ref.get('conclusion') or ''
    check('不是我们引擎的 bug' in concl,
          '结论写明"脚本的 TextBuff(0,1380,1024) 越界，原版也会中断"')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: TextBuff 上限 0x400、越界抛异常（code 0x80）与 C 侧一致')
    return 0


if __name__ == '__main__':
    sys.exit(main())