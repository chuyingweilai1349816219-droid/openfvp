#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate for ref/exe-prim-setters.json（图元字段写入指令全量扫描）。"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-prim-setters.py')
REF = os.path.join(FVP, 'ref', 'exe-prim-setters.json')
HIER = os.path.join(FVP, 'ref', 'exe-prim-hierarchy.json')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


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
    check(proc.returncode == 0, 'extract-exe-prim-setters.py --check 退出码 0')

    ref = json.loads(io.open(REF, encoding='utf-8').read())
    stores = ref.get('stores') or []
    fields = ref.get('fields') or {}

    print('== 2. 扫描结果的结构 ==')
    check(len(stores) >= 50, '写了图元字段的指令 >= 50（实际 %d）' % len(stores))
    check(all(0x43A000 <= int(s['pc'], 16) <= 0x43C900 for s in stores),
          '全部落在 0x43A000..0x43C900')
    check(all(s['offset'] in fields for s in stores),
          '每条 store 的偏移都是已知图元字段')
    names = set(s['field'] for s in stores)
    for want in ('x', 'y', 'w', 'h', 'op1', 'op2', 'angle', 'scale_x', 'scale_y'):
        check(want in names, '覆盖字段 %s' % want)

    print('== 3. 与图元结构体基线交叉核对 ==')
    if os.path.isfile(HIER):
        h = json.loads(io.open(HIER, encoding='utf-8').read())
        blob = json.dumps(h, ensure_ascii=False).lower()
        for off in ('0x14', '0x16', '0x18', '0x1a', '0x20', '0x22'):
            check(off in blob, 'exe-prim-hierarchy.json 里有偏移 %s' % off)
        check('0x34' in blob, '结构体步长 0x34 在两份基线里一致')

    print('== 4. 聚簇与未知项 ==')
    cl = ref.get('clusters') or []
    check(len(cl) == 1, '只有一簇（%d）' % len(cl))
    if cl:
        check(cl[0]['count'] == len(stores), '簇内条数 = 总条数（%d）'
              % cl[0]['count'])
        check(int(cl[0]['to'], 16) - int(cl[0]['from'], 16) < 0x600,
              '簇跨度 < 0x600 字节（handler 区很集中）')
    check(bool(ref.get('unknown')), 'unknown 里明确写了"命令 id → handler 仍未知"')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 图元字段写入指令（%d 条，集中在 %s..%s）与结构体基线一致；'
          '命令 id → handler 的映射仍列为未知'
          % (len(stores), cl[0]['from'], cl[0]['to']) if cl else 'OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())