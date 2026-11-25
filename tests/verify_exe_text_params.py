#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""文本样式命令的字段/范围门禁：必须来自 exe handler，不是猜的。

`TextFormat`(118) / `TextFunction`(119) / `TextShadowDist`(125) / `TextSkip`(127) /
`TextSpace`(128) / `TextFont`(113) 在我们的引擎里长期是桩。要让它们"操作上也一样"，
先得知道原版把它们写进**哪个字段、合法范围是多少、传非整数时是不是保持原值**。
这些只在 exe 的 handler 里。

`scripts/extract-exe-text-params.py` 用一个小数据流把这些从静态分析抄出来，冻结成
`ref/exe-text-params.json`；本门禁断言这些**硬事实**：

  * 六个命令的形态一致：arg0 = 文本层号 0..31，样式对象来自
    `settings(0x481368) + 0x98 + layer*4`；
  * `TextFormat`(118) 七参：arg1/arg2 = -32..32 → 样式对象 +0x5A8/+0x5AA；
    arg3 = 0..64 → +0x5AE；arg4 = 0..64 → +0x5AC（并把 +0x5E8 抬到见过的最大值）；
    arg5 = -16..16 → +0x5B4；arg6 = -16..16 → +0x5B0；
  * `TextFunction`(119) 三参：arg1 = 0..1 → +0x5A5；arg2 = 0..2 → +0x5A4；
    arg3 = 0..2 → +0x5A6（都是 1 字节）；
  * `TextSkip`(127)：arg1 = 0..3 → +0x5D7（1 字节）；
  * `TextSpace`(128)：arg1/arg2 = -32..32 → 与 TextFormat 的 arg1/arg2 **同一对字段**
    （+0x5A8/+0x5AA）；
  * `TextShadowDist`(125)：arg1 先钳到 0..12（`test/jge` + `mov $0xc`），再通过
    方法 `0x4248b0` 应用（本身不直接写字段）；
  * `TextFont`(113)：arg1/arg2 下限 **-5**（负数是合法特例），上限是**动态的**
    字体表长度-1（`cmp %esi,%eax`，esi 由 `settings+0x94` 的列表长度算出），
    分别调方法 `0x4247f0` / `0x424860`；
  * `TextSize`(126)：12..64 / 8..32；`TextOutSize`(120)：0..12 / 0..8；
    `TextSpeed`(129)：-1..300000。

缺原版程序清单时 SKIP 并返回 0。

用法：python tests/verify_exe_text_params.py
"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-text-params.py')
REF = os.path.join(FVP, 'ref', 'exe-text-params.json')
ASM = os.path.join(FVP, 'work', 'iroseka_HD.asm')
HANDLERS = os.path.join(FVP, 'work', 'vm_id_handler.json')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def cmds():
    with io.open(REF, encoding='utf-8') as fp:
        return json.load(fp).get('commands') or {}


def writes(cmd):
    return {(w['arg'], w['offset'], w['size']) for w in cmd.get('field_writes') or []}


def rng(cmd, arg):
    r = (cmd.get('arg_ranges') or {}).get(str(arg)) or {}
    return (r.get('min'), r.get('max'))


def calls(cmd):
    return cmd.get('calls_or_jumps') or []


def imms(cmd):
    return [(w['reg'], w['imm']) for w in cmd.get('immediate_writes') or []]


def main():
    if not os.path.isfile(ASM) or not os.path.isfile(HANDLERS):
        print('SKIP: 缺原版程序清单或 handler 表（work/ 不入库）')
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
          'extract-exe-text-params.py --check 退出码 0（实际 %d）'
          % proc.returncode)

    c = cmds()
    check(len(c) >= 9, '至少 9 个文本命令被提取（实际 %d）' % len(c))

    print('== 2. 六个命令的共同形态：层号 0..31 + settings(0x481368)+0x98+layer*4 ==')
    for cid, name in ((113, 'TextFont'), (118, 'TextFormat'),
                      (119, 'TextFunction'), (125, 'TextShadowDist'),
                      (127, 'TextSkip'), (128, 'TextSpace')):
        cmd = c.get(str(cid)) or {}
        check(cmd.get('name') == name, '%d 是 %s' % (cid, name))
        check((cmd.get('layer_table') or {}) == {'offset': 0x98, 'stride': 4},
              '%s 的文本层表是 settings+0x98、stride 4（实际 %r）'
              % (name, cmd.get('layer_table')))
        check('0x481368' in (cmd.get('globals') or []),
              '%s 用全局设置对象 0x481368' % name)
        check(rng(cmd, 0) == (0, 31),
              '%s 的层号范围 0..31（实际 %r）' % (name, rng(cmd, 0)))

    print('== 3. TextFormat(118)：7 个实参 → 6 个字段（含 +0x5E8 最大值跟踪）==')
    cmd = c.get('118') or {}
    check(cmd.get('argc') == 7, 'argc == 7（实际 %r）' % cmd.get('argc'))
    check(rng(cmd, 1) == (-32, 32) and rng(cmd, 2) == (-32, 32),
          'arg1/arg2 范围 -32..32（实际 %r / %r）' % (rng(cmd, 1), rng(cmd, 2)))
    check(rng(cmd, 3) == (0, 64) and rng(cmd, 4) == (0, 64),
          'arg3/arg4 范围 0..64（实际 %r / %r）' % (rng(cmd, 3), rng(cmd, 4)))
    check(rng(cmd, 5) == (-16, 16) and rng(cmd, 6) == (-16, 16),
          'arg5/arg6 范围 -16..16（实际 %r / %r）' % (rng(cmd, 5), rng(cmd, 6)))
    check(writes(cmd) == {(1, 0x5A8, 2), (2, 0x5AA, 2), (3, 0x5AE, 2),
                          (4, 0x5AC, 2), (4, 0x5E8, 2), (5, 0x5B4, 2),
                          (6, 0x5B0, 2)},
          '字段写入集合与现实一致（实际 %s）' % sorted(writes(cmd)))

    print('== 4. TextFunction(119)：三个 0..N 的开关 → 三个 1 字节字段 ==')
    cmd = c.get('119') or {}
    check(rng(cmd, 1) == (0, 1), 'arg1 范围 0..1（实际 %r）' % (rng(cmd, 1),))
    check(rng(cmd, 2) == (0, 2) and rng(cmd, 3) == (0, 2),
          'arg2/arg3 范围 0..2（实际 %r / %r）' % (rng(cmd, 2), rng(cmd, 3)))
    check(writes(cmd) == {(1, 0x5A5, 1), (2, 0x5A4, 1), (3, 0x5A6, 1)},
          '字段写入集合与现实一致（实际 %s）' % sorted(writes(cmd)))

    print('== 5. TextSkip(127) / TextSpace(128) ==')
    cmd = c.get('127') or {}
    check(rng(cmd, 1) == (0, 3), 'TextSkip arg1 范围 0..3（实际 %r）'
          % (rng(cmd, 1),))
    check(writes(cmd) == {(1, 0x5D7, 1)}, 'TextSkip 写 +0x5D7（1 字节）')
    cmd = c.get('128') or {}
    check(rng(cmd, 1) == (-32, 32) and rng(cmd, 2) == (-32, 32),
          'TextSpace arg1/arg2 范围 -32..32（实际 %r / %r）'
          % (rng(cmd, 1), rng(cmd, 2)))
    check(writes(cmd) == {(1, 0x5A8, 2), (2, 0x5AA, 2)},
          'TextSpace 写 +0x5A8/+0x5AA（实际 %s）' % sorted(writes(cmd)))
    tf = c.get('118') or {}
    check(writes(cmd) <= writes(tf),
          'TextSpace 的两个字段正是 TextFormat 的 arg1/arg2 字段（同一对）')

    print('== 6. TextShadowDist(125)：值先钳到 0..12，再走方法 0x4248b0 ==')
    cmd = c.get('125') or {}
    check(rng(cmd, 1)[0] == 0, 'arg1 下界 0（test/jge）；实际 %r' % (rng(cmd, 1),))
    check((('ax', 12) in imms(cmd)) and (('ax', 0) in imms(cmd)),
          '值寄存器上有 0 与 12 两个常量（钳位 0..12，实际 %s）' % imms(cmd))
    check(calls(cmd).count('jmp 0x004248b0') == 2,
          '两条路径都尾调 0x4248b0（方法应用，本身不写字段）')
    check(not writes(cmd), 'TextShadowDist 不直接写样式字段（实际 %s）'
          % sorted(writes(cmd)))

    print('== 7. TextFont(113)：下限 -5，上限动态（字体表长度-1）==')
    cmd = c.get('113') or {}
    check(rng(cmd, 1)[0] == -5 and rng(cmd, 2)[0] == -5,
          'arg1/arg2 下限 -5（实际 %r / %r）' % (rng(cmd, 1), rng(cmd, 2)))
    check('cmp    %esi,%eax' in (cmd.get('register_compares') or []),
          '上界来自寄存器比较 cmp %%esi,%%eax（字体表长度-1）')
    check('call 0x004247f0' in calls(cmd) and 'call 0x00424860' in calls(cmd),
          'arg1/arg2 分别调方法 0x4247f0 / 0x424860')

    print('== 8. 其余文本尺寸/速度命令的范围 ==')
    cmd = c.get('120') or {}
    check(rng(cmd, 1) == (0, 12) and rng(cmd, 2) == (0, 8),
          'TextOutSize 0..12 / 0..8（实际 %r / %r）'
          % (rng(cmd, 1), rng(cmd, 2)))
    cmd = c.get('126') or {}
    check(rng(cmd, 1) == (12, 64) and rng(cmd, 2) == (8, 32),
          'TextSize 12..64 / 8..32（实际 %r / %r）'
          % (rng(cmd, 1), rng(cmd, 2)))
    cmd = c.get('129') or {}
    check(rng(cmd, 1) == (-1, 300000),
          'TextSpeed -1..300000（实际 %r）' % (rng(cmd, 1),))

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 文本样式命令的字段偏移/宽度/取值范围与 exe handler 一致')
    return 0


if __name__ == '__main__':
    sys.exit(main())