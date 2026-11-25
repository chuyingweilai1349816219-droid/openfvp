#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""文本样式字段**访问地图**的门禁：谁写、谁读、读改写的算术是什么。

上一轮把"样式命令写哪个字段、范围多少"冻结成了 `ref/exe-text-params.json`；
这一轮补上"渲染侧怎么用这些字段"：`scripts/extract-exe-text-fields.py` 扫出
所有读/写/读改写点，冻结成 `ref/exe-text-fields.json`。本门禁断言这些结构性事实：

  * **写点来源干净**：九个样式字段的写点只来自两类地方 —— 样式命令的 handler
    （0x43CBB0..0x43CE70 / 0x43DA70..0x43DAE4）和一处结构拷贝例程
    （0x430E00..0x431100，把样式对象复制给排版对象）；
  * **+0x5A8 / +0x5AA / +0x5B0 / +0x5B4 只写不读**（连"被更宽读覆盖"都没有）——
    它们由拷贝例程搬到另一个对象，消费点在那边，所以"从样式对象读不到"不等于
    "没用"，这条写进文档而不是假装不存在；
  * **+0x5AC 被两处 dword 读读取**（0x430E67 / 0x4337A9），同时覆盖 +0x5AE；
  * **+0x5D7（TextSkip）** 有 2 个精确读点（0x42DB78 / 0x431084）和 3 个"逐字符
    元数据"写点（0x432394 / 0x433A70 + handler）；
  * **+0x597（TextShadowDist 写的字节）写两次、读零次**（也没有覆盖读）——
    这解释了为什么我们**不画阴影层**：不是"没实现"，是 exe 里这个字段根本没被读；
  * **+0x5E8 是"累计行宽"**：0x4247C4 / 0x4247DA 用 `add %ax,0x5e8(%ecx)` 累加，
    0x433006 用 `movswl` 读出后与 +0x5DC 比较（换行判断），
    TextFormat arg4 在 handler 里只会把它**抬高**（下限语义）。

缺原版程序清单时 SKIP 并返回 0。

用法：python tests/verify_exe_text_fields.py
"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-text-fields.py')
REF = os.path.join(FVP, 'ref', 'exe-text-fields.json')
PARAMS = os.path.join(FVP, 'ref', 'exe-text-params.json')
ASM = os.path.join(FVP, 'work', 'iroseka_HD.asm')

STYLE_FIELDS = (0x5A4, 0x5A5, 0x5A6, 0x5A8, 0x5AA, 0x5AC, 0x5AE, 0x5B0,
                0x5B4)

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def field(ref, off):
    return (ref.get('fields') or {}).get('0x%x' % off) or {}


def pcs(items):
    return [x['pc'] for x in items or []]


def main():
    if not os.path.isfile(ASM):
        print('SKIP: 缺原版程序清单 %s（work/ 不入库）' % ASM)
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
          'extract-exe-text-fields.py --check 退出码 0（实际 %d）'
          % proc.returncode)

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)

    print('== 2. 十一个字段都在地图里 ==')
    for off in STYLE_FIELDS + (0x5D7, 0x597, 0x5E8):
        check(field(ref, off).get('name') is not None,
              '+0x%x 有名字与访问记录' % off)

    print('== 3. 九个样式字段的写点只来自 handler 或结构拷贝例程 ==')
    for off in STYLE_FIELDS:
        f = field(ref, off)
        bad = [w for w in f.get('writes') or []
               if not (w.get('in_handler') or w.get('in_copy'))]
        check(not bad, '+0x%x 的写点都在 handler/copy 窗口内（越界 %s）'
              % (off, [(w['pc'], w['instr']) for w in bad]))
    expected = {0x5A4: 2, 0x5A5: 2, 0x5A6: 2, 0x5A8: 3, 0x5AA: 3,
                0x5AC: 2, 0x5AE: 2, 0x5B0: 2, 0x5B4: 2}
    for off, n in expected.items():
        check(len(field(ref, off).get('writes') or []) == n,
              '+0x%x 写点数量 = %d（实际 %d）'
              % (off, n, len(field(ref, off).get('writes') or [])))

    print('== 4. TextSpace/TextFormat 的两个字距字段：只写不读（连覆盖读都没有）==')
    for off in (0x5A8, 0x5AA, 0x5B0, 0x5B4):
        f = field(ref, off)
        check(not (f.get('reads') or []), '+0x%x 没有精确读点' % off)
        check(not (f.get('covering_reads') or []),
              '+0x%x 也没有"被更宽读覆盖"' % off)

    print('== 5. +0x5AC 被两处 dword 读读取（覆盖 +0x5AE）==')
    f = field(ref, 0x5AC)
    check(pcs(f.get('reads')) == ['0x430e67', '0x4337a9'],
          '+0x5AC 的精确读点（实际 %s）' % pcs(f.get('reads')))
    f = field(ref, 0x5AE)
    check(pcs(f.get('covering_reads')) == ['0x430e67', '0x4337a9'],
          '+0x5AE 被同一对 dword 读覆盖（实际 %s）'
          % pcs(f.get('covering_reads')))

    print('== 6. +0x5D7（TextSkip）：2 个精确读 + 3 个写（逐字符元数据）==')
    f = field(ref, 0x5D7)
    check(pcs(f.get('reads')) == ['0x42db78', '0x431084'],
          '+0x5D7 的精确读点（实际 %s）' % pcs(f.get('reads')))
    writes = f.get('writes') or []
    check(pcs(writes) == ['0x432394', '0x433a70', '0x43ce06'],
          '+0x5D7 的写点（实际 %s）' % pcs(writes))
    others = [w for w in writes if not (w.get('in_handler') or w.get('in_copy'))]
    check(len(others) == 2,
          '其中 2 个写点在排版例程里（逐字符元数据，实际 %d）' % len(others))
    cov = pcs(f.get('covering_reads'))
    check(cov == ['0x42d8ac', '0x42db17', '0x42db4a', '0x43115b'],
          '+0x5D7 被 4 个更宽的读覆盖（0x5D4..0x5D6 那组逐字符字段，实际 %s）'
          % cov)

    print('== 7. +0x597（TextShadowDist 写的字节）：写两次、读零次 ==')
    f = field(ref, 0x597)
    check(pcs(f.get('writes')) == ['0x4248b4', '0x430fdf'],
          '+0x597 的写点（实际 %s）' % pcs(f.get('writes')))
    check(not (f.get('reads') or []) and not (f.get('covering_reads') or []),
          'exe 里没有任何指令读 +0x597 —— 所以"不画阴影层"是有依据的')

    print('== 8. +0x5E8 = 累计行宽：add 累加 + movswl 读 + 换行比较 ==')
    f = field(ref, 0x5E8)
    check(pcs(f.get('rmw')) == ['0x4247c4', '0x4247da'],
          '+0x5E8 的读改写是两条 add 累加（实际 %s）' % pcs(f.get('rmw')))
    reads = pcs(f.get('reads'))
    for pc in ('0x42456f', '0x433006', '0x433029', '0x433166', '0x433467'):
        check(pc in reads, '+0x5E8 有读点 %s（实际 %s）' % (pc, reads))
    writes = f.get('writes') or []
    check('0x43ccae' in pcs(writes),
          'TextFormat(118) handler 里会写 +0x5E8（抬高下限）')
    check('0x430e72' in pcs(writes),
          '结构拷贝例程也搬这个字段')

    print('== 9. 与 ref/exe-text-params.json 交叉验证 ==')
    if not os.path.isfile(PARAMS):
        print('  skip 缺 ref/exe-text-params.json')
    else:
        with io.open(PARAMS, encoding='utf-8') as fp:
            params = json.load(fp)
        cmd = (params.get('commands') or {}).get('118') or {}
        w = {(x['arg'], x['offset']) for x in cmd.get('field_writes') or []}
        check((4, 0x5AC) in w and (4, 0x5E8) in w,
              '上一轮提取的 TextFormat arg4 同时写 0x5AC 与 0x5E8（实际 %s）'
              % sorted(w))
        check((1, 0x5A8) in w and (2, 0x5AA) in w,
              'arg1/arg2 写的就是 +0x5A8/+0x5AA（与 TextSpace 同字段）')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 文本样式字段的读/写/读改写地图与 exe 一致；'
          '阴影字节写而不读、行宽字段是累计器')
    return 0


if __name__ == '__main__':
    sys.exit(main())