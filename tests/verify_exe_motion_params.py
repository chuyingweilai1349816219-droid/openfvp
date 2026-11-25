#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Motion*/PrimSet* 实参约束的门禁：这些事实必须来自 exe 的 handler，不是猜的。

第 130 轮用"截图扫描"去判"缩放是 1.0 还是 0.9/0.8"，方法不对。本轮改成静态分析
handler：`scripts/extract-exe-motion-params.py` 把每个实参的类型检查、范围检查、
单位常量（`idiv 3600` = 角度 0.1°）、写入的 prim 字段偏移、以及调用的子例程抄出来，
冻结成 `ref/exe-motion-params.json`。本门禁断言这些**硬事实**：

  MotionMoveS2(54)  argc=8
    arg0 图元 1..0xFFF ／ arg1..arg4 缩放 100..10000（per-mille，1.0 = 1000）
    arg5 时长 1..300000 ms ／ arg6 **缓动 mode 1..5** ／ arg7 非整数=布尔标志
    最终调用 0x0042d670（注册补间）
  MotionMoveS2Stop(55)  只有 arg0，跳 0x00427460（**取消动画、保持当前值**，不回滚）
  MotionAlpha(42)       arg1/arg2 起始/目标 alpha 0..255（非 int 或越界 = 沿用当前 prim+7）；
                        arg3 时长 1..300000 ms；arg4 0..1；arg5 布尔；调 0x0042d460 注册
  PrimSetAlpha(77)      写 prim+7（alpha 字节），值 0..255
  PrimSetBlend(78)      写 prim+8（混合模式字节），值 0..1；prim 0..0xFFF
  PrimSetXY(90)         写 prim+0x14/+0x16；**哪个实参不是 int 就不写那个字段**（保持原值）
  PrimSetWH(89)         写 prim+0x18/+0x1a，同样按实参逐个判 int；脏位 0x1|0x40
  PrimSetRS(82)         写入 prim+0x24 角度（单位 idiv 3600）、+0x26/+0x28 缩放
  PrimSetZ(91)          写入 prim+0x12

这几条正好回答第 130 轮那个矛盾：脚本 `MotionMoveS2(258,1300,900,…,7,3)` 的时长是
**7 ms**（几乎瞬时），`Stop` 又只是取消——所以它属于"离开标题时的缩小"，
标题稳态就是 1.0，与我们渲染器一致。

缺原版程序清单或 handler 表时 SKIP 并返回 0。

用法：python tests/verify_exe_motion_params.py
"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-motion-params.py')
REF = os.path.join(FVP, 'ref', 'exe-motion-params.json')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def ranges_of(cmd):
    return {(r['arg'], r['min'], r['max']) for r in cmd.get('arg_ranges') or []}


def main():
    asm = os.path.join(FVP, 'work', 'iroseka_HD.asm')
    tab = os.path.join(FVP, 'work', 'vm_id_handler.json')
    if not os.path.isfile(asm) or not os.path.isfile(tab):
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
          'extract-exe-motion-params.py --check 退出码 0（实际 %d）'
          % proc.returncode)

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)
    cmds = ref.get('commands') or {}

    print('== 2. MotionMoveS2(54)：8 个实参 + mode 1..5 ==')
    c = cmds.get('54') or {}
    check(c.get('argc') == 8, 'argc == 8（实际 %r）' % c.get('argc'))
    rs = ranges_of(c)
    check((0, 1, 0xFFF) in rs, 'arg0 图元 1..0xFFF（实际 %s）' % sorted(rs))
    for arg in (1, 2, 3, 4):
        check((arg, 100, 10000) in rs,
              'arg%d 缩放 100..10000（实际 %s）' % (arg, sorted(rs)))
    check((5, 1, 300000) in rs, 'arg5 时长 1..300000（实际 %s）' % sorted(rs))
    check((6, 1, 5) in rs, 'arg6 缓动 mode 1..5（实际 %s）' % sorted(rs))
    check('call 0x0042d670' in (c.get('calls_or_jumps') or []),
          '最终调用 0x0042d670（补间注册）')

    print('== 3. MotionMoveS2Stop(55)：只取消，不回滚 ==')
    c = cmds.get('55') or {}
    check(c.get('argc') == 1, 'argc == 1（实际 %r）' % c.get('argc'))
    check('jmp 0x00427460' in (c.get('calls_or_jumps') or []),
          '跳到共享的取消例程 0x00427460（实际 %s）'
          % (c.get('calls_or_jumps'),))

    print('== 4. MotionAlpha(42)：起始/目标 alpha 各钳到 255，时长 1..300000 ==')
    c = cmds.get('42') or {}
    check(c.get('argc') == 6, 'argc == 6（实际 %r）' % c.get('argc'))
    rs = ranges_of(c)
    check((0, 1, 0xFFF) in rs, 'arg0 图元 1..0xFFF（实际 %s）' % sorted(rs))
    check((1, 0, 255) in rs and (2, 0, 255) in rs,
          'arg1/arg2 两个 alpha 值都钳到 0..255（实际 %s）' % sorted(rs))
    check((3, 1, 300000) in rs, 'arg3 时长 1..300000 ms（实际 %s）' % sorted(rs))
    check((4, 0, 1) in rs, 'arg4 是 0..1 的标志（实际 %s）' % sorted(rs))

    print('== 5. PrimSetRS(82) / PrimSetZ(91)：字段偏移与单位 ==')
    c = cmds.get('82') or {}
    check(c.get('split_constants') == [3600],
          '角度单位常量 3600（0.1°，实际 %r）' % (c.get('split_constants'),))
    offs = sorted(f['field_off'] for f in c.get('struct_field_writes') or [])
    check(offs == [0x24, 0x26, 0x28],
          '写入 prim 字段 0x24/0x26/0x28（实际 %s）' % [hex(o) for o in offs])
    c = cmds.get('91') or {}
    offs = sorted(f['field_off'] for f in c.get('struct_field_writes') or [])
    check(offs == [0x12], 'PrimSetZ 写入 prim+0x12（实际 %s）'
          % [hex(o) for o in offs])

    print('== 6. PrimSetXY(90)/WH(89)：非 int 实参 = 保持原值；写哪个字段 ==')
    def cond_writes(cmd):
        return {(w['arg'], w['field_off']) for w in
                (cmd.get('conditional_field_writes') or [])
                if w.get('only_if_int')}

    c = cmds.get('90') or {}
    check(cond_writes(c) == {(1, 0x14), (2, 0x16)},
          'PrimSetXY：arg1→prim+0x14(x)、arg2→prim+0x16(y)，且仅当是 int（实际 %s）'
          % sorted(cond_writes(c)))
    c = cmds.get('89') or {}
    check(cond_writes(c) == {(1, 0x18), (2, 0x1a)},
          'PrimSetWH：arg1→prim+0x18(w)、arg2→prim+0x1a(h)，且仅当是 int（实际 %s）'
          % sorted(cond_writes(c)))
    check(c.get('dirty_flag_bits') == [0x1, 0x40],
          'PrimSetWH 置 dirty 位 0x1|0x40（实际 %s）'
          % [hex(b) for b in c.get('dirty_flag_bits') or []])
    c = cmds.get('90') or {}
    check(c.get('dirty_flag_bits') == [0x40],
          'PrimSetXY 只置 dirty 位 0x40（实际 %s）'
          % [hex(b) for b in c.get('dirty_flag_bits') or []])

    print('== 7. PrimSetOP(81) 只对 type==4 的图元生效；PrimSetBlend(78) 范围 ==')
    c = cmds.get('81') or {}
    gates = [g['value'] for g in (c.get('type_gates') or [])
             if g.get('kind') == 'prim_type']
    check(gates == [4], 'PrimSetOP 的图元类型门 == 4（实际 %s）' % gates)
    check(cmds.get('81', {}).get('dirty_flag_bits') == [0x40],
          'PrimSetOP 置 dirty 位 0x40')
    c = cmds.get('78') or {}
    rs = ranges_of(c)
    check((0, 0, 0xFFF) in rs, 'PrimSetBlend arg0 0..0xFFF（实际 %s）' % sorted(rs))
    check((1, 0, 1) in rs, 'PrimSetBlend arg1 0..1（实际 %s）' % sorted(rs))
    absf = [f['field_off'] for f in c.get('abs_field_writes') or []]
    check(absf == [0x8], 'PrimSetBlend 直接写 prim+8 混合字节（实际 %s）'
          % [hex(o) for o in absf])

    print('== 8. PrimSetAlpha(77)：直接写 prim+7 的 alpha 字节，值 0..255 ==')
    c = cmds.get('77') or {}
    absf = [f['field_off'] for f in c.get('abs_field_writes') or []]
    check(absf == [0x7], 'PrimSetAlpha 写 prim+7（实际 %s）' % [hex(o) for o in absf])
    rs = ranges_of(c)
    check((0, 1, 0x1000) in rs,
          'PrimSetAlpha arg0 1..0x1000（上界在 asm 里是 jge，即有效 1..0xFFF；实际 %s）'
          % sorted(rs))

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: Motion*/PrimSet* 的实参约束与 exe handler 一致（帧扫描不再是依据）')
    return 0


if __name__ == '__main__':
    sys.exit(main())