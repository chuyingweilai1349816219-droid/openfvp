#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""脚本侧实参审计的门禁：命令表的 argc 必须与**真实脚本的调用点**一致。

为什么要有它：命令表的 argc 是从 HCB 内嵌元数据里读的，读法一偏，VM 就会弹错
个数的实参（栈失衡），而这类错在序章基线里可能根本不触发。本门禁换一个独立
证据源对账——脚本自己在每个 `syscall` 之前实际压了几个值。

做法：`scripts/audit-script-argc.py` 对两部作品的全部指令做一遍基本块回溯，
把能干净判定的调用点（exact）与表里的 argc 比；本门禁只做断言与冻结：

  1. `audit-script-argc.py --check`：重新审计的结果与 `ref/script-argc.json`
     **逐字节一致**（解码器/统计逻辑漂移立刻红）；
  2. 两部作品的异常清单必须**恰好**是已知的 3 条，且 id 相同：
     `IntToText(24)` / `TextFontCount(114)` / `TextFontName(116)`。
     这 3 条在**两部作品里完全相同**、每条只有 1–2 个调用点，成因是表达式临时值
     留在栈上（`pushtemp` / 前一条 syscall 的返回值），不是表错——所以冻结成
     已知项，任何**新增**异常都会失败；
  3. `TitleMenu(141)`：表里的 argc 是 **1**，且它在脚本里每个调用点的**紧邻前一条
     指令都是 `pushtrue`**（两部作品分别 6 处 / 7 处）。这条把"TitleMenu 靠 6 个
     参数传布局"的猜测正式否掉：它就是"进/不进标题菜单"的一个布尔。

缺 HCB 时打印 SKIP 并返回 0（与其它依赖游戏数据的门禁同口径）。

用法：python tests/verify_script_argc.py
"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
AUDIT = os.path.join(FVP, 'scripts', 'audit-script-argc.py')
REF = os.path.join(FVP, 'ref', 'script-argc.json')
GAMES = {
    'iroseka': os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                            'iroseka.hcb'),
    'hoshimemo': os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed',
                              'Hoshimemo_HD.hcb'),
}
KNOWN_ANOMALIES = {24, 114, 116}   # IntToText / TextFontCount / TextFontName

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def main():
    missing = [n for n, p in GAMES.items() if not os.path.isfile(p)]
    if missing:
        print('SKIP: 缺游戏数据 %s（这是可选语料，见 docs/status.md）' % missing)
        return 0
    if not os.path.isfile(REF):
        print('FAIL: 缺基线 %s' % REF)
        return 1

    print('== 1. 重新审计并与基线逐字节比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', AUDIT, '--game', 'all', '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=900)
    out = proc.stdout.decode('utf-8', 'replace')
    print(out.rstrip())
    check(proc.returncode == 0,
          'audit-script-argc.py --check 退出码 0（实际 %d）' % proc.returncode)

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)

    print('== 2. 异常清单：只允许已知的 3 条，且两部作品一致 ==')
    seen = {}
    for name in sorted(GAMES):
        r = ref['games'].get(name)
        if not check(r is not None, '%s 有审计结果' % name):
            continue
        ids = sorted(a['id'] for a in r.get('anomalies') or [])
        seen[name] = ids
        check(ids == sorted(KNOWN_ANOMALIES),
              '%s 的异常清单 == %s（实际 %s）'
              % (name, sorted(KNOWN_ANOMALIES), ids))
        per = r['per_command']
        matched = sum(1 for v in per.values() if v['verdict'] == 'match')
        bad = [k for k, v in per.items()
               if v['verdict'] not in ('match', 'no-exact-sites', 'mismatch')]
        check(not bad, '%s 没有未知 verdict（实际 %s）' % (name, bad))
        check(matched >= 80,
              '%s 至少 80 条命令的 argc 被脚本调用点证实（实际 %d）' % (name, matched))
        print('   %s: 指令 %d 条，可判定调用点 %d 个，证实 %d 条命令'
              % (name, r['instructions'], r['exact_sites'], matched))
    if len(seen) == 2:
        check(seen['iroseka'] == seen['hoshimemo'],
              '两部作品的异常清单完全相同（iroseka=%s hoshimemo=%s）'
              % (seen['iroseka'], seen['hoshimemo']))

    print('== 3. TitleMenu(141)：argc=1，紧邻前一条指令是 pushtrue ==')
    for name in sorted(GAMES):
        r = ref['games'].get(name) or {}
        f = (r.get('focus') or {}).get('141')
        if not check(f is not None, '%s 记录了 TitleMenu 的检查' % name):
            continue
        check(f['table_argc'] == 1,
              '%s：表里 TitleMenu argc == 1（实际 %r）' % (name, f['table_argc']))
        hist = f.get('prev_instr_hist') or {}
        check(list(hist.keys()) == ['pushtrue'],
              '%s：TitleMenu 的所有调用点紧邻前一条都是 pushtrue（实际 %r）'
              % (name, hist))
        check(sum(hist.values()) >= 5,
              '%s：TitleMenu 调用点 >= 5 个（实际 %d）'
              % (name, sum(hist.values())))

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 命令表 argc 与两部作品的真实调用点一致（仅 3 条已知表达式临时值异常）')
    return 0


if __name__ == '__main__':
    sys.exit(main())