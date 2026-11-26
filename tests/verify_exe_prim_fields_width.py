#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate for ref/exe-prim-fields-width.json（图元字段 int16 的读侧证据）。"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-prim-fields-width.py')
REF = os.path.join(FVP, 'ref', 'exe-prim-fields-width.json')
SETTERS = os.path.join(FVP, 'ref', 'exe-prim-setters-args.json')
SYSCALL = os.path.join(FVP, 'src', 'syscall.c')

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
    check(proc.returncode == 0, 'extract-exe-prim-fields-width.py --check 退出码 0')

    ref = json.loads(io.open(REF, encoding='utf-8').read())
    fields = ref.get('fields') or []

    print('== 2. 十个字段都有 movswl 读 ==')
    check(len(fields) == 10, '十个字段（实际 %d）' % len(fields))
    names = [f['field'] for f in fields]
    for want in ('x', 'y', 'w', 'h', 'u', 'v', 'op1', 'op2', 'angle', 'scale'):
        check(want in names, '含字段 %s' % want)
    for f in fields:
        check('movswl' in f['read_instr'],
              '%s 的读指令是 movswl（%s）' % (f['field'], f['read_pc']))
        check('u16' in f['write_width'], '%s 写侧是 u16' % f['field'])
    check(ref.get('range') == [-32768, 32767], '范围 = -32768..32767')

    print('== 3. 与 setter 基线交叉核对 ==')
    if os.path.isfile(SETTERS):
        st = json.loads(io.open(SETTERS, encoding='utf-8').read())
        known = set(h['handler'] for h in st.get('handlers') or [])
        for f in fields:
            for h in f['written_by']:
                check(h in known, '%s 的写侧 handler %s 在 setter 基线里'
                      % (f['field'], h))

    print('== 4. C 侧按 int16 截断 ==')
    if os.path.isfile(SYSCALL):
        src = io.open(SYSCALL, encoding='utf-8').read()
        check('FVP_S16' in src and 'int16_t' in src,
              'syscall.c 里有 int16 截断宏')
        for case in ('case 81:', 'case 89:', 'case 90:'):
            k = src.index(case)
            check('FVP_S16' in src[k:k + 900], '%s 用了 FVP_S16' % case)

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 图元字段的 int16 语义有写侧（u16）+ 读侧（movswl）双边证据，'
          'C 侧已按此截断')
    return 0


if __name__ == '__main__':
    sys.exit(main())