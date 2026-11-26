#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate for ref/exe-graph-load.json（GraphLoad(22) 实参规则）+ C 侧一致性。"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-graph-load.py')
REF = os.path.join(FVP, 'ref', 'exe-graph-load.json')
USAGE = os.path.join(FVP, 'ref', 'command-usage.json')
SYSCALL = os.path.join(FVP, 'src', 'syscall.c')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def main():
    if not os.path.isfile(os.path.join(FVP, 'work', 'iroseka_HD.asm')):
        print('SKIP: 缺反汇编清单')
        return 0
    if not os.path.isfile(REF):
        print('FAIL: 缺基线 %s' % REF)
        return 1

    print('== 1. 重新提取并与基线逐字节比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', EXTRACT, '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=600)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0, 'extract-exe-graph-load.py --check 退出码 0')

    ref = json.loads(io.open(REF, encoding='utf-8').read())
    print('== 2. 规则本身 ==')
    check(ref.get('name') == 'GraphLoad' and ref.get('id') == 22, '是 GraphLoad(22)')
    check(ref.get('argc') == 2, 'argc = 2')
    check((ref.get('arg0') or {}).get('range') == [0, 0xFFF],
          'arg0 = 槽号 0..0xFFF')
    disp = ((ref.get('arg1') or {}).get('dispatch') or {})
    check('0x424150' in (disp.get('type0') or ''), 'type 0 → 释放（0x424150）')
    check('0x68DFAC' in (disp.get('type4') or ''), 'type 4 → 数据段基址 + 偏移')
    check('0x6A49EC' in (disp.get('type5') or ''), 'type 5 → 0x6A49EC 串表')
    check('0x428BD0' in (ref.get('then') or ''), '加载走 0x428BD0')
    check(bool(ref.get('unknown')), 'unknown 保留后处理语义')

    print('== 3. 与使用矩阵互证 ==')
    if os.path.isfile(USAGE):
        u = json.loads(io.open(USAGE, encoding='utf-8').read())
        for g in ('iroseka', 'hoshimemo'):
            cmds = {c['id']: c for c in u['games'][g]['commands']}
            c = cmds.get(22) or {}
            check(c.get('name') == 'GraphLoad' and c.get('handler') == '0x43a340',
                  '%s 的 cmd 22 = GraphLoad → 0x43A340' % g)
            check(c.get('count', 0) > 100, '%s 里 GraphLoad 用了 %d 次'
                  % (g, c.get('count', 0)))

    print('== 4. C 侧 ==')
    src = io.open(SYSCALL, encoding='utf-8').read()
    k = src.index('case 22:')
    seg = src[k:k + 900]
    check('fvp_value_is_null' in seg, 'case 22 判 type 0')
    check('gfx_graph_load(g, id, NULL)' in seg, 'case 22 对 type 0 调释放')
    check('FVP_STR' in seg, 'case 22 保留字符串加载路径')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: GraphLoad(22) 的实参规则已冻结；type 0 = 释放槽已实现')
    return 0


if __name__ == '__main__':
    sys.exit(main())
