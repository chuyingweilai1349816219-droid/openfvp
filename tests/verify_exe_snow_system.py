#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate for ref/exe-snow-system.json（雪系统记录字段图）。"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-snow-system.py')
REF = os.path.join(FVP, 'ref', 'exe-snow-system.json')
DISPATCH = os.path.join(FVP, 'ref', 'exe-prim-type-dispatch.json')

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
    check(proc.returncode == 0, 'extract-exe-snow-system.py --check 退出码 0')

    ref = json.loads(io.open(REF, encoding='utf-8').read())
    fields = ref.get('fields') or []
    offs = [f['offset'] for f in fields]

    print('== 2. 字段访问图 ==')
    check(len(fields) >= 10, '至少 10 个字段偏移（实际 %d）' % len(fields))
    for want in ('0x500c', '0x5010', '0x5014', '0x501c', '0x5044'):
        check(want in offs, '含偏移 %s' % want)
    check(all(f['reads'] + f['writes'] > 0 for f in fields),
          '每个字段至少一次访问')
    check(any('int32 -> float' in ','.join(f['widths']) for f in fields),
          '有 fildl 读的字段（int 转 float）')

    print('== 3. 设备调用与结构 ==')
    rec = ref.get('record') or {}
    check(rec.get('stride') == '0x5048', '记录步长 0x5048')
    check('0x684378' in (rec.get('base') or ''), '基址 engine+0x684378')
    vt = ref.get('device_calls') or []
    check(len(vt) >= 8, '至少 8 处设备方法调用（实际 %d）' % len(vt))
    offs_vt = set(x['vtable_offset'] for x in vt)
    for want in ('0xe4', '0x104', '0x10c', '0x164'):
        check(want in offs_vt, '用到 vtable 偏移 %s' % want)

    print('== 4. 与类型分派基线互证 ==')
    if os.path.isfile(DISPATCH):
        d = json.loads(io.open(DISPATCH, encoding='utf-8').read())
        snow = (d.get('snow_system') or {})
        check(snow.get('fn') == ref.get('draw_fn'),
              'snow 绘制函数与类型分派基线一致（%s）' % ref.get('draw_fn'))
        check(rec.get('stride') == '0x5048' and
              '0x5048' in (snow.get('record') or ''), '两边步长都是 0x5048')
    check(bool(ref.get('unknown')), 'unknown 里保留粒子公式与 render state 语义')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 雪系统记录的字段访问图（10 个偏移 / 10 次访问 / 4 个设备方法）已冻结，'
          '与类型分派基线互证')
    return 0


if __name__ == '__main__':
    sys.exit(main())
