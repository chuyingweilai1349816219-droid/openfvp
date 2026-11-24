#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""syscall 覆盖率静态检查：148 条命令里哪些真的有处理分支。

为什么需要它：运行时直方图（外壳汇总里的 `unimpl cmds`）只覆盖"脚本实际调到的
命令"。脚本没走到的命令就算缺席也看不出来 —— 2026-11-24 之前"146/148 已实现"
这个说法就是这么来的（把没人调用的 29 条当成了已实现）。这个脚本直接扫
`src/syscall.c` 里四个 `switch (cid)` 块：

    fvp_gfx_argc   —— 只是 argc 修正，**不算**实现
    fvp_sys_gfx    —— 渲染转发（Prim*/Text*/Graph* 等）
    fvp_sys_snd    —— 音频命令
    fvp_syscall    —— 主分发

覆盖 = 后三个 switch 里出现 ∪ 通用 `*Test` 规则 ∪ Allowlist（已确证的原版空操作）。

用法：
    python fvp/scripts/check-commands.py
    python fvp/scripts/check-commands.py --json out/cmd-coverage.json
    python fvp/scripts/check-commands.py --strict   # 有缺口就返回 1
退出码：0 = 无缺口（或非 --strict），1 = --strict 下有缺口，2 = 环境问题。
"""
import io
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
SYSCALL = os.path.join(FVP, 'src', 'syscall.c')
TABLE = os.path.join(FVP, 'src', 'gen_cmd_table.h')

# 已确证的原版空操作（docs/vm-commands.md §5）：有结论，不需要实现分支。
ALLOWLIST = {
    41: 'verified no-op',
    108: 'verified no-op',
    141: 'verified no-op',
}

# 已知缺口数（2026-11-24 审计）。这是"棘轮"：实现掉一条就把这个数改小；
# 缺口数一旦变大（有人删了 case 或换了生成表），--strict 会失败。
MISSING_BASELINE = 26

BLOCK_NAMES = ['fvp_gfx_argc', 'fvp_sys_gfx', 'fvp_sys_snd', 'fvp_syscall']
HANDLER_BLOCKS = BLOCK_NAMES[1:]     # 前三个之外的都算"有处理"


def read_names():
    s = io.open(TABLE, encoding='utf-8').read()
    blk = s.split('fvp_cmd_names')[1].split('};')[0]
    return re.findall(r'"([A-Za-z0-9_]+)"', blk)


def switch_blocks(src):
    """把每个 `switch (cid) {` … 配对的 `}` 抽出来，按出现顺序返回。"""
    blocks = []
    for m in re.finditer(r'switch \(cid\) \{', src):
        i = m.end() - 1          # 指向 '{'
        depth = 0
        j = i
        while j < len(src):
            if src[j] == '{':
                depth += 1
            elif src[j] == '}':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        blocks.append(src[i:j + 1])
    return blocks


def cases_in(block):
    return set(int(x) for x in re.findall(r'case (\d+):', block))


def main():
    argv = sys.argv[1:]
    strict = '--strict' in argv
    out_json = None
    if '--json' in argv:
        i = argv.index('--json')
        if i + 1 < len(argv):
            out_json = argv[i + 1]

    names = read_names()
    n = len(names)
    src = io.open(SYSCALL, encoding='utf-8').read()
    blocks = switch_blocks(src)
    if n == 0 or len(blocks) < len(BLOCK_NAMES):
        print('check-commands: cannot parse (names=%d switches=%d)'
              % (n, len(blocks)))
        return 2

    per_block = {}
    for name, blk in zip(BLOCK_NAMES, blocks):
        per_block[name] = cases_in(blk)

    handlers = set()
    for name in HANDLER_BLOCKS:
        handlers |= per_block[name]
    argc_only = per_block['fvp_gfx_argc'] - handlers

    generic, allowed, missing = [], [], []
    for cid in range(n):
        if cid in handlers:
            continue
        if names[cid].endswith('Test'):
            generic.append(cid)
        elif cid in ALLOWLIST:
            allowed.append(cid)
        else:
            missing.append(cid)

    covered = n - len(generic) - len(allowed) - len(missing)
    print('commands             %d' % n)
    print('handler in a switch  %d' % covered)
    print('generic *Test rule   %d' % len(generic))
    print('documented no-op     %d' % len(allowed))
    print('MISSING              %d' % len(missing))
    if argc_only:
        print('(argc-only, not handlers: %s)'
              % ', '.join(str(c) for c in sorted(argc_only)))
    if missing:
        for cid in missing:
            print('  missing %3d  %s' % (cid, names[cid]))
    else:
        print('\nRESULT: PASS -- every command id has a handler, a generic '
              '*Test rule, or a documented no-op')

    if len(missing) > MISSING_BASELINE:
        print('\nREGRESSION: %d missing > baseline %d (a handler disappeared?)'
              % (len(missing), MISSING_BASELINE))
    elif len(missing) < MISSING_BASELINE:
        print('\nNOTE: %d missing < baseline %d -- 把 MISSING_BASELINE 改小'
              % (len(missing), MISSING_BASELINE))

    result = {
        'total': n,
        'with_handler': covered,
        'generic_test': len(generic),
        'documented_noop': len(allowed),
        'missing': [{'id': cid, 'name': names[cid]} for cid in missing],
        'argc_only': sorted(argc_only),
        'pass': not missing,
    }
    if out_json:
        io.open(out_json, 'w', encoding='utf-8').write(
            json.dumps(result, ensure_ascii=False, indent=1))
        print('json -> %s' % out_json)
    if missing and strict:
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())