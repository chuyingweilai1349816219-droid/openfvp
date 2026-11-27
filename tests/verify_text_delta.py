#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""`--faithful-ops --cmp-matrix` 与冻结文本基线之间的**差异门禁**。

2026-11-27 的现状：忠实语义已经把行尾 `」`（宽松 eq 比出的装饰）和行首 `~`
（eq 表 C 的 (0,0) 被判成假）都修掉了，剩下的差异是 18 个块、净 -2 行，
而且**全部落在两类**：

  null-text  行形如 `[层] 0` —— 本该是台词的位置把 null 当文本打了出来；
  nameplate  层 8 的名牌行 —— `　真紅　` 对上基线的 ` 真紅／悠馬 `。

这条门禁把"今天差多少、差在哪一类"钉住：任何改动只要改变了差异块数或类别，
就必须重新分类（而不是悄悄漂移）。它不断言"已经对齐"。
"""
import argparse
import importlib.util
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
REF = os.path.join(FVP, 'ref', 'text-delta-faithful.json')
BASELINE = os.path.join(FVP, 'ref', 'ref-900k.txt')
EXTRACTOR = os.path.join(FVP, 'scripts', 'diff-text-baseline.py')
FVM = os.path.join(FVP, 'bin', 'fvm.exe')
HCB = r'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\iroseka.hcb'


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
        print('verify_text_delta: FAIL missing %s' % a.json)
        return 1
    with open(a.json, encoding='utf-8') as f:
        ref = json.load(f)

    print('== 冻结的差异描述')
    check(ref.get('schema') == 'fvp-text-delta/1', 'schema fvp-text-delta/1')
    s = ref.get('summary', {})
    check(s.get('blocks') == 18, '差异 18 块（实际 %s）' % s.get('blocks'))
    check(s.get('delta_lines') == -2, '净行数 -2（实际 %s）' % s.get('delta_lines'))
    cats = s.get('by_category') or {}
    check('quote' not in cats, '没有「」」类差异（宽松 eq 的装饰已修）')
    check('tilde' not in cats, '没有 `~` 类差异（eq 表 C (0,0) 已修）')
    check(set(cats) <= {'nameplate', 'null-text'},
          '差异只剩 nameplate / null-text（实际 %s）' % ','.join(sorted(cats)))
    blocks = ref.get('blocks') or []
    check(all(b.get('categories') for b in blocks), '每块都带分类')
    null_only = [b for b in blocks if b['categories'] == ['null-text']]
    both = [b for b in blocks if set(b['categories']) == {'nameplate', 'null-text'}]
    check(len(null_only) + len(both) == len(blocks),
          '块分类只有两种形态（%d 纯 null-text + %d 带名牌）'
          % (len(null_only), len(both)))

    print('== 重新跑一遍忠实语义并重算差异')
    if not (os.path.isfile(FVM) and os.path.isfile(HCB)):
        print('  skip  bin/fvm.exe 或 iroseka.hcb 不存在')
    else:
        fd, path = tempfile.mkstemp(suffix='.txt')
        os.close(fd)
        try:
            subprocess.run([FVM, HCB, '--steps', '900000', '--click', '600',
                            '--quiet', '--faithful-ops', '--cmp-matrix',
                            '--text-out', path],
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=600)
            mod = load_module('diff_text_baseline', EXTRACTOR)
            got = mod.build(path, BASELINE)
            check(got['summary'] == s, 'summary 逐字段相等')
            check(got['blocks'] == blocks, '18 个差异块逐字段相等')
        finally:
            os.unlink(path)

    if fails:
        print('verify_text_delta: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_text_delta: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())