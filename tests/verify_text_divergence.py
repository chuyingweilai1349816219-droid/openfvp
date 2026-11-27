#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""`TextPrint` 第一处分歧的门禁（`ref/text-print-divergence.json`）。

`tests/verify_text_delta.py` 钉住"差多少块"，这条钉住"**第一处**差在哪"：
跑两遍（默认 / `--faithful-ops --cmp-matrix`）打开 `FVP_DBG_TEXT`，按行号对齐
`TextPrint` 日志，比较 (pc, layer, text)。

当前冻结的第一处分歧：第 33 条，`pc=0x3A547`（对话路径，模板调用点在
`0x3A539`），默写下 `g226` 是字符串、忠实语义下是 int 0 —— 也就是"这一行的正文
根本没有被组装出来"。上一处（第 2 条）只有栈槽类型不同，正文一致，单独计数。
"""
import argparse
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
REF = os.path.join(FVP, 'ref', 'text-print-divergence.json')
EXTRACTOR = os.path.join(FVP, 'scripts', 'trace-text-prints.py')


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--json', default=REF)
    a = ap.parse_args()
    fails = []

    def check(cond, msg):
        print(('  ok    ' if cond else '  FAIL  ') + msg)
        if not cond:
            fails.append(msg)

    if not os.path.isfile(a.json):
        print('verify_text_divergence: FAIL missing %s' % a.json)
        return 1
    with open(a.json, encoding='utf-8') as f:
        ref = json.load(f)

    print('== 冻结的第一处分歧')
    check(ref.get('schema') == 'fvp-text-print-divergence/1',
          'schema fvp-text-print-divergence/1')
    fd = ref.get('first_divergence') or {}
    check(fd.get('index') == 33, '分歧在第 33 条 TextPrint（实际 %s）'
          % fd.get('index'))
    leg = fd.get('legacy') or {}
    fai = fd.get('faithful') or {}
    check(leg.get('pc') == fai.get('pc') == '0x3a547',
          '两边同一个调用点 0x3A547')
    check(leg.get('layer') == fai.get('layer') == 0, '都是 layer 0（正文）')
    check((leg.get('types') or {}).get('g226') == 2,
          '默认语义下 g226 是字符串（type 2）')
    check((fai.get('types') or {}).get('g226') == 0,
          '忠实语义下 g226 是 int（type 0）→ 正文没组装出来')
    check(fai.get('text') == '0', '忠实语义下打出来的就是 "0"')
    check((leg.get('text') or '').startswith('おはよう'),
          '默认语义下是真正的台词（おはよう…）')
    check(ref.get('type_only_diffs', 0) >= 1,
          '另有"只有栈槽类型不同、正文一致"的记录（%s 条）'
          % ref.get('type_only_diffs'))

    print('== 重新跑两遍对账')
    mod = load_module('trace_text_prints', EXTRACTOR)
    got = mod.build()
    if got is None:
        print('  skip  bin/fvm.exe 或 iroseka.hcb 不存在')
    else:
        check(got.get('first_divergence') == fd, '第一处分歧逐字段相等')
        check(got.get('type_only_diffs') == ref.get('type_only_diffs'),
              'type-only 计数相等')
        check(got.get('legacy_prints') == ref.get('legacy_prints') and
              got.get('faithful_prints') == ref.get('faithful_prints'),
              '两遍的 TextPrint 条数相等')

    if fails:
        print('verify_text_divergence: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_text_divergence: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())