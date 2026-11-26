#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate for ref/exe-colorset.json（ColorSet(7) = 调色板写入）+ C 侧一致性。"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-colorset.py')
REF = os.path.join(FVP, 'ref', 'exe-colorset.json')
LOAD = os.path.join(FVP, 'ref', 'exe-load-flow.json')
SYSCALL = os.path.join(FVP, 'src', 'syscall.c')
VM = os.path.join(FVP, 'src', 'vm.c')

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
    check(proc.returncode == 0, 'extract-exe-colorset.py --check 退出码 0')

    ref = json.loads(io.open(REF, encoding='utf-8').read())
    pal = ref.get('palette') or {}

    print('== 2. 调色板结构 ==')
    check(ref.get('name') == 'ColorSet' and ref.get('id') == 7, '是 ColorSet(7)')
    check(ref.get('argc') == 5, 'argc = 5')
    check(ref.get('index_range') == [0, 0xFF], 'index 0..0xFF')
    check(pal.get('base') == 'engine+0x6A44C0', '调色板基址 engine+0x6A44C0')
    check(pal.get('entries') == 256, '256 项')
    check(pal.get('order') == 'BGRA', 'BGRA 顺序')
    check((pal.get('component_offsets') or {}) ==
          {'B': 0, 'G': 1, 'R': 2, 'A': 3}, '分量偏移 B0/G1/R2/A3')
    disp = ref.get('dispatch') or {}
    check(disp.get('R') == 'arg1 -> +2' and disp.get('B') == 'arg3 -> +0',
          'R/G/B 的参数对应（arg1=R→+2、arg3=B→+0）')
    check(any('type==2' in r for r in (ref.get('rules') or [])),
          '规则写明逐分量判 int')

    print('== 3. 与读档基线互证 ==')
    if os.path.isfile(LOAD):
        lf = json.loads(io.open(LOAD, encoding='utf-8').read())
        steps = lf.get('frame_consumers', {}).get('load_apply', {}).get(
            'restore_steps') or []
        hit = [s for s in steps if '0x6A44C0' in (s.get('what') or '')]
        check(bool(hit), '读档恢复顺序里有 0x6A44C0 那一步')
        if hit:
            check('调色板' in hit[0]['what'],
                  '那一步已标注为"调色板"（本轮纠正了旧的"图元 id 表"）')

    print('== 4. C 侧 ==')
    src = io.open(SYSCALL, encoding='utf-8').read()
    k = src.index('case 7:')
    seg = src[k:k + 1200]
    check('fvp_vm_color_set' in seg, 'case 7 调 fvp_vm_color_set')
    check('idx >= 0 && idx <= 0xFF' in seg, 'case 7 校验 index 0..0xFF')
    vm = io.open(VM, encoding='utf-8').read()
    vi = vm.index('fvp_vm_color_set')
    vseg = vm[vi:vi + 1400]
    check('palette[index][0]' in vseg and 'palette[index][3]' in vseg,
          'BGRA 四个分量都写进 palette')
    check('has_b' in vseg and 'has_r' in vseg, '逐分量 has_* 控制')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: ColorSet(7) = 写 256×BGRA 调色板（engine+0x6A44C0），C 侧已实现')
    return 0


if __name__ == '__main__':
    sys.exit(main())
