#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_prim_refs.py -- 把"图元 265 在 HCB 里被哪些地方引用"冻结成可查清单。

背景（见 CHANGELOG 八十三/八十四续）：标题菜单在 `0x592DF` 等
`MotionAlphaTest(265)` 的动作记录，而 265 的初始化/动画代码散在十几处。
这条门禁用 `scripts/scan-prim-refs.py` 静态扫 iroseka 的 HCB，断言：

  1. 265 的引用点总数（五彩斑斓 = 38）；
  2. 关键位置都在：
       - `0x58EF8`：批次初始化里对 265 调 helper `0x235C9`；
       - `0x592DF`：等它的循环（`pushint16 265` 后紧跟 `syscall 44`）；
       - `0x59355`：循环之后的 `MotionAlpha(265, 0, 255, …)`；
  3. 三个"会跑批次初始化"的调用点 `0x59E93/0x59EBB/0x59EFC` 指向 `0x584D8`。

只做静态扫描，不跑游戏；缺游戏数据时 SKIP 并返回 0（与其它 verify_* 一致）。
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SCAN = os.path.join(ROOT, 'scripts', 'scan-prim-refs.py')
HCB = os.path.join(r'C:\Users\Mechrevo\Downloads',
                   '\u4e94\u5f69\u6591\u6593\u7684\u4e16\u754cHD', 'iroseka.hcb')

fails = []


def check(ok, msg):
    if ok:
        print('  ok   %s' % msg)
    else:
        print('  FAIL %s' % msg)
        fails.append(msg)


def main():
    if not os.path.isfile(SCAN):
        print('SKIP: scripts/scan-prim-refs.py missing')
        return 0
    if not os.path.isfile(HCB):
        print('SKIP: game data missing (%s)' % HCB)
        return 0
    tmp = os.path.join(ROOT, 'out', 'prim-265.json')
    os.makedirs(os.path.dirname(tmp), exist_ok=True)
    rc = subprocess.call([sys.executable, SCAN, '265', '--json', tmp, '--quiet'])
    if rc != 0 or not os.path.isfile(tmp):
        print('FAIL: scanner exited %s / no json' % rc)
        return 1
    data = json.load(open(tmp, encoding='utf-8'))
    sites = {s['offset']: s for s in data['sites']}
    print('-- 1. 引用点总数 --')
    check(data['count'] == 38, '265 共 38 个引用点（实测 %d）' % data['count'])

    print('-- 2. 关键位置 --')
    init = sites.get('0x58ef8')
    check(init is not None, '批次初始化里有 265（0x58EF8）')
    if init is not None:
        names = [i['name'] for i in init['instr']]
        check('call' in names and 144841 in
              [i['args'][0] for i in init['instr'] if i['name'] == 'call'],
              '0x58EF8 后面调的是 helper 0x235C9（%d）' % 144841)

    loop = sites.get('0x592df')
    check(loop is not None, '等待循环在 0x592DF')
    if loop is not None:
        seq = loop['instr']
        check(len(seq) >= 2 and seq[1]['name'] == 'syscall' and
              seq[1]['args'] == [44], '0x592DF 紧跟 syscall 44（MotionAlphaTest）')

    fade = sites.get('0x59355')
    check(fade is not None, '循环之后的 265 淡入在 0x59355')

    print('-- 3. 批次初始化函数的四个调用点 --')
    data2 = open(HCB, 'rb').read()
    want = {0x590CC, 0x59E93, 0x59EBB, 0x59EFC}
    import struct
    pat = struct.pack('<I', 0x584D8)
    found = set()
    i = 0
    while True:
        i = data2.find(pat, i)
        if i < 0:
            break
        found.add(i - 1)   # 前一字节是 opcode 0x02(call)
        i += 1
    check(found == want,
          'call 0x584D8 的四个调用点 = %s（实测 %s）'
          % (sorted('%05x' % x for x in want),
             sorted('%05x' % x for x in found)))
    print('  note: 我们只执行到 0x590CC（arg=1，跳过批次）；另外三个从未执行')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 265 的引用清单与四个调用点都符合冻结事实')
    return 0


if __name__ == '__main__':
    sys.exit(main())