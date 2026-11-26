#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate for ref/exe-variant-types.json（全局变量 Variant 类型语义基线）。"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-variant-types.py')
REF = os.path.join(FVP, 'ref', 'exe-variant-types.json')
SER = os.path.join(FVP, 'ref', 'exe-save-serialize.json')
LOAD = os.path.join(FVP, 'ref', 'exe-load-flow.json')
SRC = os.path.join(FVP, 'src', 'platform', 'win32', 'main_win32.c')

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
    check(proc.returncode == 0, 'extract-exe-variant-types.py --check 退出码 0')

    ref = json.loads(io.open(REF, encoding='utf-8').read())
    types = {t['type']: t for t in ref.get('types') or []}

    print('== 2. 七个类型都在 ==')
    check(sorted(types) == list(range(7)), '类型 0..6 齐全（实际 %r）'
          % sorted(types))
    for t, want in ((0, 'null'), (2, 'int'), (4, '偏移'), (5, '槽号'), (6, '索引')):
        check(want in (types[t]['name'] if t in types else ''), 
              'type %d 的名字含 %r' % (t, want))

    print('== 3. type6 的 handler 证据 ==')
    ev = ref.get('type6_evidence') or {}
    check(ev.get('handler') == '0x445cb0', '证据 handler = 0x445CB0')
    instrs = ' '.join(x.get('instr') or '' for x in ev.get('instrs') or [])
    check('$0x6' in instrs and '0x6a4df0' in instrs,
          '指令里有 cmpb $0x6 与 0x6A4DF0 寻址')
    check('0x14' in (ev.get('table') or ''), '表项大小 0x14')

    print('== 4. 与写/读档基线交叉核对 ==')
    if os.path.isfile(SER):
        ser = json.loads(io.open(SER, encoding='utf-8').read())
        seg14 = [s for s in ser.get('segments') or []
                 if s.get('order') == 14]
        check(bool(seg14) and '0x6a4df0' in json.dumps(seg14[0]).lower(),
              '写档第 14 段就是 0x6A4DF0 表（256 项）')
        check(bool(seg14) and '256' in json.dumps(seg14[0], ensure_ascii=False),
              '第 14 段写明 256 项')
    if os.path.isfile(LOAD):
        lf = json.loads(io.open(LOAD, encoding='utf-8').read())
        steps = lf.get('frame_consumers', {}).get('load_apply', {}).get(
            'restore_steps') or []
        check(any('0x6A4DF0' in (s.get('what') or '') for s in steps),
              '读档恢复顺序里有 0x6A4DF0（步 7）')

    print('== 5. 存档数据证据 ==')
    saves = ref.get('type6_saves') or []
    check(len(saves) >= 4, '至少 4 份存档解出 type6（实际 %d）' % len(saves))
    for s in saves:
        check(s.get('dense') and s.get('lt256'),
              '%s：type6 载荷是 0..%d 连续整数且 < 256'
              % (s.get('save'), s.get('max')))
    ss = ref.get('type4_strings') or []
    check(len(ss) >= 3, 'type4 至少 3 个偏移解出字符串（实际 %d）' % len(ss))
    check(any('voice/' in (x.get('text') or '') for x in ss),
          '其中一个 type4 偏移解出 "voice/..."')

    print('== 6. C 侧一致性（哪些类型已回灌）==')
    if os.path.isfile(SRC):
        src = io.open(SRC, encoding='utf-8').read()
        app = ref.get('applied') or {}
        for t in app.get('read_restart') or []:
            pat = {0: 'type == 0', 2: 'type == 2', 4: 'type == 4',
                   5: 'type == 5'}.get(t)
            if pat:
                check(pat in src, '--load-restart 里有 type %d 的处理分支' % t)
        for t in app.get('not_applied') or []:
            if t == 6:
                check('type 6' not in src and 'type == 6' not in src,
                      'type 6 确实没有回灌分支（要先恢复 0x6A4DF0 表）')
        check('type 1/3/6 still unmapped' in src,
              '日志写明 type 1/3/6 仍未映射')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: Variant 类型语义（7 个类型，每条带证据）与 exe/存档/写读档基线一致')
    return 0


if __name__ == '__main__':
    sys.exit(main())