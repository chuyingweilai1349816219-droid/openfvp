#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""门禁：两部作品实际用到的命令，引擎必须**全部实现**（多作品兼容性矩阵）。"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
AUDIT = os.path.join(FVP, 'scripts', 'audit-command-usage.py')
REF = os.path.join(FVP, 'ref', 'command-usage.json')
SYSCALL = os.path.join(FVP, 'src', 'syscall.c')
DISPATCH = os.path.join(FVP, 'ref', 'exe-cmd-dispatch.json')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def main():
    for p in (r'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\iroseka.hcb',
              r'C:\Users\Mechrevo\Downloads\hoshimemo_installed\Hoshimemo_HD.hcb'):
        if not os.path.isfile(p):
            print('SKIP: 缺游戏数据 %s' % p)
            return 0
    if not os.path.isfile(REF):
        print('FAIL: 缺基线 %s' % REF)
        return 1

    print('== 1. 重新审计并与基线逐字节比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', AUDIT, '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=1200)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0, 'audit-command-usage.py --check 退出码 0')

    ref = json.loads(io.open(REF, encoding='utf-8').read())
    games = ref.get('games') or {}

    print('== 2. 两部作品的命令集合 ==')
    check(sorted(games) == ['hoshimemo', 'iroseka'], '两部作品都有记录')
    for name, g in sorted(games.items()):
        check(g['distinct_ids'] == 131, '%s 用到 131 个命令（实际 %d）'
              % (name, g['distinct_ids']))
        check(g['syscalls'] > 4000, '%s syscall 总数 > 4000（实际 %d）'
              % (name, g['syscalls']))
        missing = g.get('missing_in_engine') or []
        check(not missing, '%s 引擎未实现 0 条（实际 %d：%r）'
              % (name, len(missing), missing[:5]))
        ids = [c['id'] for c in g['commands']]
        check(ids == sorted(ids), '%s 的命令 id 有序' % name)
        check(all(0 <= cid <= 147 for cid in ids),
              '%s 的 id 都在 0..147' % name)

    print('== 3. 与命令分派基线互证 ==')
    if os.path.isfile(DISPATCH):
        d = json.load(io.open(DISPATCH, encoding='utf-8'))
        known = set(c['id'] for c in d.get('commands') or [])
        unknown = set()
        for g in games.values():
            for c in g['commands']:
                if c['handler'] == '?':
                    unknown.add(c['id'])
        # AudioLoad(0) 是分派表 147/148 里唯一没连上的那条（注册形态不同），
        # 这里允许它缺席，其它任何 id 缺席都算失败。
        check(unknown <= {0}, '除 AudioLoad(0) 外都能在分派表里找到 handler（缺 %r）'
              % sorted(unknown - {0})[:5])
        check(0 in unknown, 'AudioLoad(0) 确实仍不在分派表里（已知项）')
        check(len(known) >= 147, '分派表 >= 147 条')

    print('== 4. C 侧：PartsMotionTest(69) 已实现 ==')
    if os.path.isfile(SYSCALL):
        src = io.open(SYSCALL, encoding='utf-8', errors='replace').read()
        check('case 69:' in src, 'syscall.c 有 case 69')
        k = src.index('case 69:')
        seg = src[k:k + 500]
        check('fvp_vm_parts_motion_test' in seg, 'case 69 调 parts_motion_test')
        vm = io.open(os.path.join(FVP, 'src', 'vm.c'), encoding='utf-8').read()
        i = vm.index('fvp_vm_parts_motion_test')
        seg2 = vm[i:i + 800]
        check('id < 1 || id > 0x3F' in seg2, '槽号校验 1..0x3F')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 两部作品用到的 131 条命令引擎全部实现（含本轮补上的 PartsMotionTest(69)）')
    return 0


if __name__ == '__main__':
    sys.exit(main())