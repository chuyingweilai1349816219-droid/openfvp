#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate for ref/exe-motion-tests.json（七条 *Test 的容器表）+ C 侧接线。"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-motion-tests.py')
REF = os.path.join(FVP, 'ref', 'exe-motion-tests.json')
LOAD = os.path.join(FVP, 'ref', 'exe-load-flow.json')
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
    check(proc.returncode == 0, 'extract-exe-motion-tests.py --check 退出码 0')

    ref = json.loads(io.open(REF, encoding='utf-8').read())
    tests = {t['id']: t for t in ref.get('tests') or []}

    print('== 2. 七条 *Test ==')
    for cid in (44, 47, 50, 53, 56, 59, 145):
        check(cid in tests, '含命令 %d（%s）' % (cid, tests.get(cid, {}).get('name')))
    for cid, t in tests.items():
        if cid == 145:
            check(t.get('id_range') == [],
                  'V3DMotionTest 没有 id 参数（实际 %r）' % (t.get('id_range'),))
            check(bool(t.get('note')), 'V3DMotionTest 的这条差异写进了 note')
        else:
            check(t.get('id_range') == [1, 0xFFF],
                  '%s 的 id 范围 1..0xFFF（实际 %r）'
                  % (t['name'], t.get('id_range')))
        check(bool(t.get('container')), '%s 有容器偏移' % t['name'])
        check(bool(t.get('query_fn')), '%s 有查询函数' % t['name'])
    containers = set(t['container'] for t in tests.values())
    check(len(containers) == 7, '七个容器互不相同（实际 %d）' % len(containers))

    print('== 3. 与读档恢复顺序互证 ==')
    if os.path.isfile(LOAD):
        lf = json.loads(io.open(LOAD, encoding='utf-8').read())
        steps = lf.get('frame_consumers', {}).get('load_apply', {}).get(
            'restore_steps') or []
        blob = ' '.join((s.get('what') or '') for s in steps)
        for c in ('0x651E04', '0x65380C', '0x679A5C', '0x67CE64',
                  '0x680A6C', '0x683F04'):
            check(c in blob, '读档恢复顺序里有容器 %s' % c)

    print('== 4. C 侧接线 ==')
    if os.path.isfile(SYSCALL):
        src = io.open(SYSCALL, encoding='utf-8').read()
        for cid, kind in ((44, 'FVP_TWEEN_ALPHA'), (50, 'FVP_TWEEN_MOVE_XY'),
                          (56, 'FVP_TWEEN_SCALE')):
            k = src.index('case %d:' % cid)
            seg = src[k:k + 900]
            check('gfx_motion_test' in seg, 'case %d 用 gfx_motion_test' % cid)
            check(kind in seg, 'case %d 用 %s' % (cid, kind))
        check('fvp_motion_test' in io.open(
            os.path.join(FVP, 'src', 'motion.c'), encoding='utf-8').read(),
            'motion.c 有 fvp_motion_test')
        check(bool(ref.get('unknown')), 'unknown 里保留容器内部格式与未接线的四条')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 七条 *Test 的容器表已冻结，其中 44/50/56 已接到真实现（ALPHA/MOVE/SCALE）')
    return 0


if __name__ == '__main__':
    sys.exit(main())
