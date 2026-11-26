#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""文本排版「笔位链」门禁（ref/exe-text-layout.json）。

被验证的主张：

  1. `TextFormat(118)` 的 arg1/arg2 写 `tw+0x5A8/+0x5AA`，`TextSpace(128)` 写的
     也是这两个 —— 但这两个字段在原版里**没有任何读点**（连更宽读都没有），
     所以它们的效果是零：我们只记录、不渲染是对的；
  2. 真正被读的是 arg4：写 `tw+0x5AC`，复位时（`0x430E72`）抄进 `tw+0x5E8`
     （笔位/累计行宽），随后被逐字推进（`0x4247C4`/`0x4247DA` 的 `add`）、
     参与换行判据（`0x433011` 比较），换行后恢复行首值（`0x4330A8`）；
  3. 排版器给的字符 x 也会写进笔位（`0x432B02`）。

有反汇编 `work/iroseka_HD.asm` 时重新抽取逐字段比对；缺失时只校验冻结 JSON 与
独立的 `ref/exe-text-fields.json`（不伪造通过）。
"""
import argparse
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
REF = os.path.join(FVP, 'ref', 'exe-text-layout.json')
FIELDS = os.path.join(FVP, 'ref', 'exe-text-fields.json')
EXTRACTOR = os.path.join(FVP, 'scripts', 'extract-exe-text-layout.py')

EXPECT_SITES = [
    ('0x430e67', 'mov    0x5ac(%esi),%ax'),
    ('0x430e72', 'mov    %ax,0x5e8(%esi)'),
    ('0x4247c4', 'add    %ax,0x5e8(%ecx)'),
    ('0x4247da', 'add    %ax,0x5e8(%ecx)'),
    ('0x432b02', 'mov    %cx,0x5e8(%esi)'),
    ('0x433006', 'movswl 0x5e8(%esi),%ecx'),
    ('0x433011', 'cmp    %edx,%ecx'),
    ('0x4330a8', 'mov    %cx,0x5e8(%esi)'),
]


def load_extractor():
    spec = importlib.util.spec_from_file_location('extract_text_layout', EXTRACTOR)
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
        print('verify_exe_text_layout: FAIL missing %s' % a.json)
        return 1
    with open(a.json, encoding='utf-8') as f:
        ref = json.load(f)

    print('== 冻结基线自洽性')
    check(ref.get('schema') == 'fvp-text-layout/1', 'schema fvp-text-layout/1')
    sites = ref.get('sites', [])
    check([(s['pc'], s['expect']) for s in sites] == EXPECT_SITES,
          '8 个站点按顺序（pc + 指令文本）')
    check(all(s.get('ok') for s in sites), '每个站点都标注 ok')
    check(all(s.get('role') for s in sites), '每个站点都有角色说明')

    print('== 字段读点（死存储 vs 真被用）')
    f = ref.get('fields', {})
    check(f.get('0x5a8', {}).get('reads') == [], '+0x5A8 无读点（TextSpace arg1）')
    check(f.get('0x5a8', {}).get('covering') == [], '+0x5A8 连覆盖读都没有')
    check(f.get('0x5aa', {}).get('reads') == [], '+0x5AA 无读点（TextSpace arg2）')
    check(f.get('0x5aa', {}).get('covering') == [], '+0x5AA 连覆盖读都没有')
    check(f.get('0x5ac', {}).get('reads') == ['0x430e67', '0x4337a9'],
          '+0x5AC 有 2 个读点（实际 %s）' % f.get('0x5ac', {}).get('reads'))

    print('== 死存储清单（写了但全 exe 没人读）')
    dead = ref.get('dead_fields', [])
    check([d['offset'] for d in dead] ==
          ['0x5a4', '0x5a5', '0x5a6', '0x5a8', '0x5aa', '0x5ae', '0x5b0', '0x5b4'],
          '8 个死存储字段（TextFunction 3 + TextFormat 5）')
    check(all(not d['reads'] for d in dead), '没有任何精确读点')
    check(all(not d['rmw'] for d in dead), '也没有读改写（add/sub/inc…）')
    counts = {d['offset']: len(d['writes']) for d in dead}
    check(counts.get('0x5a8') == 3 and counts.get('0x5aa') == 3,
          '0x5A8/0x5AA 各 3 个写点（handler + TextSpace + 结构拷贝）')
    check(all(counts.get(o) == 2 for o in
              ('0x5a4', '0x5a5', '0x5a6', '0x5ae', '0x5b0', '0x5b4')),
          '其余 6 个各 2 个写点（handler + 结构拷贝）')

    print('== 独立来源对账（ref/exe-text-fields.json）')
    if os.path.isfile(FIELDS):
        with open(FIELDS, encoding='utf-8') as fp:
            fields = json.load(fp)['fields']
        for off, key in (('0x5a8', '0x5a8'), ('0x5aa', '0x5aa'), ('0x5ac', '0x5ac')):
            e = fields.get(key, {})
            check(not e.get('reads') and not e.get('covering_reads') if off != '0x5ac'
                  else [r['pc'] for r in e.get('reads', [])] == ['0x430e67', '0x4337a9'],
                  '%s 与字段表一致' % off)
    else:
        print('  skip  field map not present')

    print('== 重新抽取反汇编，逐字段比对')
    mod = load_extractor()
    got = mod.build()
    if got is None:
        print('  skip  work/iroseka_HD.asm not present')
    else:
        check(got['sites'] == sites, '站点表逐字段相等')
        check(got['fields'] == f, '字段读点相等')
        check(got.get('dead_fields') == dead, '死存储清单逐字段相等')
        check(got['claim'] == ref.get('claim'), '结论文字未变')

    if fails:
        print('verify_exe_text_layout: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_exe_text_layout: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())