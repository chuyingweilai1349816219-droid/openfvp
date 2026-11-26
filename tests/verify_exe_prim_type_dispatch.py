#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate for ref/exe-prim-type-dispatch.json（图元类型分派 + snow 结构）。"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-prim-type-dispatch.py')
REF = os.path.join(FVP, 'ref', 'exe-prim-type-dispatch.json')
TEXT_DRAW = os.path.join(FVP, 'ref', 'exe-text-draw.json')
LOAD = os.path.join(FVP, 'ref', 'exe-load-flow.json')
CMDS = os.path.join(FVP, 'ref', 'exe-cmd-dispatch.json')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def main():
    exe = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                       'iroseka_HD.exe')
    if not os.path.isfile(exe) or not os.path.isfile(
            os.path.join(FVP, 'work', 'iroseka_HD.asm')):
        print('SKIP: 缺 exe 或反汇编清单')
        return 0
    if not os.path.isfile(REF):
        print('FAIL: 缺基线 %s' % REF)
        return 1

    print('== 1. 重新提取并与基线逐字节比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', EXTRACT, '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=600)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0, 'extract-exe-prim-type-dispatch.py --check 退出码 0')

    ref = json.loads(io.open(REF, encoding='utf-8').read())
    types = {t['type']: t for t in ref.get('types') or []}

    print('== 2. 七个类型 ==')
    check(sorted(types) == list(range(1, 8)), 'type 1..7 齐全（实际 %r）'
          % sorted(types))
    names = {t: types[t]['name'] for t in types}
    check(names.get(2) == 'tile' and names.get(4) == 'sprite' and
          names.get(5) == 'text' and names.get(7) == 'snow',
          '类型名 2/4/5/7 = tile/sprite/text/snow')
    check(types[3]['target'] == types[6]['target'],
          'type 3 与 type 6 共用同一个（空）落点')

    print('== 3. 与已冻结的绘制基线互证 ==')
    if os.path.isfile(TEXT_DRAW):
        td = json.loads(io.open(TEXT_DRAW, encoding='utf-8').read())
        blob = json.dumps(td, ensure_ascii=False).lower()
        check('42ae40' in blob, '文字绘制基线里也是 0x42AE40（type 5 落点）')
    if os.path.isfile(LOAD):
        lf = json.loads(io.open(LOAD, encoding='utf-8').read())
        steps = lf.get('frame_consumers', {}).get('load_apply', {}).get(
            'restore_steps') or []
        check(any('0x684378' in (s.get('what') or '') for s in steps),
              '读档恢复顺序里有 0x684378（雪系统容器）')
    if os.path.isfile(CMDS):
        cd = json.loads(io.open(CMDS, encoding='utf-8').read())
        by = {c['id']: c for c in cd.get('commands') or []}
        check((by.get(84) or {}).get('name') == 'PrimSetSnow',
              '命令表里 cmd 84 = PrimSetSnow')

    print('== 4. snow 结构 ==')
    snow = ref.get('snow_system') or {}
    check(snow.get('fn') == '0x42aa10', 'snow 绘制函数 0x42AA10')
    check('0x2E' in (snow.get('selector') or ''), '用 prim+0x2E 选雪系统')
    check('0x5048' in (snow.get('record') or ''), '记录步长 0x5048')
    check('0x684378' in (snow.get('record') or ''), '记录基址 engine+0x684378')
    check(bool(ref.get('unknown')), 'unknown 里保留雪系统字段语义')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 图元类型分派（type 1..7）与 snow 结构已冻结，'
          '并与文字绘制/读档恢复/命令表三边互证')
    return 0


if __name__ == '__main__':
    sys.exit(main())