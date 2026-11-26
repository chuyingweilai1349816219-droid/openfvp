#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""七条 `*Test` 命令的 handler 结构（A 类：ref/exe-motion-tests.json）。

它们共用同一个形状：

    mov    0x4(%esp),%eax
    cmpb   $0x2,(%eax)          ; arg0 必须是 int
    mov    0x4(%eax),%eax
    cmp    $0x1,%eax  /  cmp $0xfff,%eax   ; id 必须在 1..0xFFF
    ...
    lea    0x68dfc0(%edx,%ecx,1),%esi      ; 线程对象
    push   %eax
    add    $0x<container>,%ecx             ; 每个 Test 用不同的"动作容器"
    call   0x42xxxx                        ; 查询：还在跑吗
    test   %eax,%eax / setne %al
    mov    %al,0x814(%esi)                 ; **无条件**写返回值（1 = 还在跑）

本脚本把这七个 handler 的 (命令, handler, 容器, 查询函数) 抄成表。
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-motion-tests.json')

LINE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')

CMDS = [
    (44, 'MotionAlphaTest', 0x43A9C0),
    (47, 'MotionAnimTest', 0x43AB10),
    (50, 'MotionMoveTest', 0x43ACE0),
    (53, 'MotionMoveRTest', 0x43AF20),
    (56, 'MotionMoveS2Test', 0x43B0D0),
    (59, 'MotionMoveZTest', 0x43AE00),
    (145, 'V3DMotionTest', 0x437CE0),
]


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)
    if not os.path.isfile(ASM):
        print('SKIP: 缺反汇编清单 %s' % ASM)
        return 0
    rows = {}
    for line in io.open(ASM, encoding='utf-8', errors='replace'):
        m = LINE.match(line.rstrip('\n'))
        if m:
            rows[int(m.group(1), 16)] = m.group(3).split('#')[0].strip()

    out = []
    for cid, name, addr in CMDS:
        # 在 handler 后 0x90 字节内找 `add $0x...,%ecx` 与 `call 0x42xxxx`
        container = None
        call = None
        id_range = None
        has_setne = False
        for a in range(addr, addr + 0x98, 1):
            ins = rows.get(a)
            if ins is None:
                continue
            m = re.match(r'^add\s+\$(0x[0-9a-f]+),%ecx$', ins)
            if m and container is None:
                container = m.group(1)
            m = re.match(r'^call\s+(0x42[0-9a-f]{4})$', ins)
            if m and call is None:
                call = m.group(1)
            if re.match(r'^cmp\s+\$0x1,%eax$', ins) and id_range is None:
                id_range = [1]
            if re.match(r'^cmp\s+\$0xfff,%eax$', ins) and id_range is not None:
                if 0xFFF not in id_range:
                    id_range.append(0xFFF)
            if ins.startswith('setne'):
                has_setne = True
        note = None
        if cid == 145:
            note = ('无 id 参数：直接查 engine+0x683F04 这个 3D 动作容器，'
                    '结果照旧写线程 +0x814')
            id_range = []
        out.append({'id': cid, 'name': name, 'handler': '0x%06x' % addr,
                    'container': container, 'query_fn': call,
                    'id_range': id_range,
                    'writes_result_unconditionally': has_setne,
                    'note': note})

    result = {
        'note': '七条 *Test 命令的 handler 结构（A 类）：id 1..0xFFF、各用不同容器、'
                '查询后无条件把结果写进线程 +0x814（1 = 还在跑）；由 '
                'scripts/extract-exe-motion-tests.py 生成',
        'result_field': '线程 +0x814（VM 返回值）',
        'tests': out,
        'unknown': ['各容器（engine+0x651E04 / 0x65BA54 / 0x680A6C …）内部的动作记录格式'],
    }
    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    print('== 七条 *Test ==')
    for t in out:
        print('  %-3d %-16s %-10s container=%-12s query=%s  setne=%s'
              % (t['id'], t['name'], t['handler'],
                 t['container'] or '?', t['query_fn'] or '?',
                 t['writes_result_unconditionally']))
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        if io.open(OUT_JSON, encoding='utf-8').read() != text:
            print('FAIL: %s 与提取结果不一致' % OUT_JSON)
            return 1
        print('OK: %s 与提取结果逐字节一致' % OUT_JSON)
        return 0
    io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n').write(text)
    print('written %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())