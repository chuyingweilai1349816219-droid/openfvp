#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 里抄出**文本类 handler 的实参规则**（A 类）。

本轮覆盖四个 handler，规则全部是"逐分量 cmpb type==2 + 范围检查，不满足就跳过
那一个分量"这一族：

  * TextBuff(110)  0x43C830：层号 0..0x1F；w/h 缺省 8，int 且 >=0 才覆盖；
  * TextColor(112) 0x43C8D0：层号 0..0x1F；R/G/B 各自 int 且 0..0xFF，否则保持原值；
  * TextOutSize(120) 0x43C960：层号 0..0x1F；arg2 0..0xC、arg3 0..0x8，逐个跳过；
  * TextSize(126)  0x43CD60：层号 0..0x1F；arg2 0xC..0x40、arg3 0x8..0x20，逐个跳过。

产物 `ref/exe-text-handlers.json`。

用法：
    python scripts/extract-exe-text-handlers.py            # 打印 + 写 json
    python scripts/extract-exe-text-handlers.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-text-handlers.json')

LINE_RE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')

# (handler pc, command id, 名称, 关键指令 pc → 期望助记符)
HANDLERS = [
    (0x43C830, 110, 'TextBuff', [
        (0x43C834, 'cmpb   $0x2,(%eax)'),
        (0x43C840, 'cmp    $0x1f,%ecx'),
        (0x43C858, 'mov    $0x8,%edi'),
        (0x43C861, 'mov    0xc(%eax),%edx'),
        (0x43C87B, 'call   0x430ea0'),
    ]),
    (0x43C8D0, 112, 'TextColor', [
        (0x43C8D5, 'cmpb   $0x2,(%esi)'),
        (0x43C8E1, 'cmp    $0x1f,%eax'),
        (0x43C8FA, 'mov    0xc(%esi),%eax'),
        (0x43C901, 'cmp    $0xff,%eax'),
        (0x43C918, 'mov    0x14(%esi),%eax'),
        (0x43C934, 'mov    0x1c(%esi),%eax'),
    ]),
    (0x43C960, 120, 'TextOutSize', [
        (0x43C965, 'cmpb   $0x2,(%esi)'),
        (0x43C971, 'cmp    $0x1f,%eax'),
        (0x43C991, 'cmp    $0xc,%eax'),
        (0x43C9AD, 'cmp    $0x8,%eax'),
    ]),
    (0x43DA70, 113, 'TextFont', [
        (0x43DA75, 'cmpb   $0x2,(%edi)'),
        (0x43DA81, 'cmp    $0x1f,%ecx'),
        (0x43DAA3, 'sar    $0x2,%esi'),
        (0x43DAA6, 'dec    %esi'),
        (0x43DAB0, 'cmp    $0xfffffffb,%eax'),
        (0x43DAB5, 'cmp    %esi,%eax'),
        (0x43DAD8, 'call   0x424860'),
    ]),
    (0x43CBB0, 125, 'TextShadowDist', [
        (0x43CBB4, 'cmpb   $0x2,(%edx)'),
        (0x43CBC0, 'cmp    $0x1f,%eax'),
        (0x43CBDA, 'mov    0xc(%edx),%eax'),
        (0x43CBE1, 'xor    %eax,%eax'),
        (0x43CBEC, 'cmp    $0xc,%eax'),
        (0x43CBFA, 'jmp    0x4248b0'),
    ]),
    (0x43CDD0, 127, 'TextSkip', [
        (0x43CDD4, 'cmpb   $0x2,(%eax)'),
        (0x43CDE0, 'cmp    $0x1f,%ecx'),
        (0x43CDFF, 'cmp    $0x3,%edx'),
        (0x43CE06, 'mov    %al,0x5d7(%ecx)'),
    ]),
    (0x43CE10, 128, 'TextSpace', [
        (0x43CE14, 'cmpb   $0x2,(%eax)'),
        (0x43CE20, 'cmp    $0x1f,%ecx'),
        (0x43CE3B, 'cmp    $0xffffffe0,%edx'),
        (0x43CE40, 'cmp    $0x20,%edx'),
        (0x43CE45, 'mov    %dx,0x5a8(%ecx)'),
        (0x43CE62, 'mov    %ax,0x5aa(%ecx)'),
    ]),
    (0x43CC10, 118, 'TextFormat', [
        (0x43CC17, 'cmp    %bl,(%eax)'),
        (0x43CC2A, 'cmp    $0x1f,%ecx'),
        (0x43CC48, 'cmp    $0xffffffe0,%edx'),
        (0x43CC7E, 'cmp    $0x40,%edx'),
        (0x43CC9E, 'cmp    %dx,0x5e8(%ecx)'),
        (0x43CCBD, 'cmp    $0xfffffff0,%edx'),
        (0x43CCD6, 'cmp    $0xfffffff0,%edx'),
    ]),
    (0x43CCF0, 119, 'TextFunction', [
        (0x43CCF4, 'cmpb   $0x2,(%eax)'),
        (0x43CD00, 'cmp    $0x1f,%ecx'),
        (0x43CD1F, 'cmp    $0x1,%edx'),
        (0x43CD37, 'cmp    $0x2,%edx'),
        (0x43CD4F, 'cmp    $0x2,%edx'),
    ]),
    (0x43CD60, 126, 'TextSize', [
        (0x43CD65, 'cmpb   $0x2,(%esi)'),
        (0x43CD71, 'cmp    $0x1f,%eax'),
        (0x43CD8D, 'cmp    $0xc,%eax'),
        (0x43CD92, 'cmp    $0x40,%eax'),
        (0x43CDAA, 'cmp    $0x8,%eax'),
        (0x43CDAF, 'cmp    $0x20,%eax'),
    ]),
]

RULES = {
    '110': {'layer': 'int 0..0x1F', 'w': '缺省 8；int 且 >=0 才覆盖',
            'h': '缺省 8；int 且 >=0 才覆盖', 'out_of_range': '越界走 0x430EA0 抛异常'},
    '112': {'layer': 'int 0..0x1F', 'r': 'int 0..0xFF，否则保持原值',
            'g': 'int 0..0xFF，否则保持原值', 'b': 'int 0..0xFF，否则保持原值',
            'out_of_range': '逐分量跳过，不钳位'},
    '120': {'layer': 'int 0..0x1F', 'a': 'int 0..0xC，否则保持原值',
            'b': 'int 0..0x8，否则保持原值', 'out_of_range': '逐分量跳过'},
    '126': {'layer': 'int 0..0x1F', 'w': 'int 0xC..0x40，否则保持原值',
            'h': 'int 0x8..0x20，否则保持原值', 'out_of_range': '逐分量跳过'},
    '113': {'layer': 'int 0..0x1F',
            'font1': 'int 且 -5..(字体表项数-1)，否则保持原值',
            'font2': 'int 且 -5..(字体表项数-1)，否则保持原值',
            'out_of_range': '逐分量跳过；上界是**动态**的（0x43DAA3/0x43DAA6：(end-begin)/4-1）'},
    '125': {'layer': 'int 0..0x1F',
            'dist': '**钳位**到 0..0xC（负数→0、>0xC→0xC），非 int→0',
            'out_of_range': '钳位（不是跳过）'},
    '127': {'layer': 'int 0..0x1F', 'mode': 'int 0..3，否则保持原值',
            'out_of_range': '跳过'},
    '128': {'layer': 'int 0..0x1F', 'x': 'int -0x20..0x20，否则保持原值',
            'y': 'int -0x20..0x20，否则保持原值', 'out_of_range': '逐分量跳过'},
    '118': {'layer': 'int 0..0x1F',
            'a1': 'tw+0x5A8，int -0x20..0x20',
            'a2': 'tw+0x5AA，int -0x20..0x20',
            'a3': 'tw+0x5AE，int 0..0x40',
            'a4': 'tw+0x5AC，int 0..0x40；新值比 pen(0x5E8) 大时一并抬高 pen',
            'a5': 'tw+0x5B4，int -0x10..0x10',
            'a6': 'tw+0x5B0，int -0x10..0x10',
            'out_of_range': '逐分量跳过（不满足的分量保持原值）'},
    '119': {'layer': 'int 0..0x1F', 'a': 'tw+0x5A5，int 0..1',
            'b': 'tw+0x5A4，int 0..2', 'c': 'tw+0x5A6，int 0..2',
            'out_of_range': '逐分量跳过'},
}


def load_rows(path):
    rows = {}
    with io.open(path, encoding='utf-8', errors='replace') as fp:
        for line in fp:
            m = LINE_RE.match(line.rstrip('\n'))
            if m:
                rows[int(m.group(1), 16)] = m.group(3).split('#')[0].strip()
    return rows


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)
    if not os.path.isfile(ASM):
        print('SKIP: 缺原版程序清单 %s' % ASM)
        return 0
    rows = load_rows(ASM)
    out = []
    checked = []
    for hpc, cid, name, expect in HANDLERS:
        for addr, want in expect:
            got = rows.get(addr)
            if got is None:
                print('FAIL: 0x%x 不存在（期望 %r）' % (addr, want))
                return 1
            if got != want:
                print('FAIL: 0x%x = %r（期望 %r）' % (addr, got, want))
                return 1
            checked.append({'pc': '0x%06x' % addr, 'instr': want})
        out.append({'id': cid, 'name': name, 'handler': '0x%06x' % hpc,
                    'rules': RULES[str(cid)]})

    result = {
        'note': '从 exe 静态分析抄出的文本类 handler 实参规则（A 类）；'
                '由 scripts/extract-exe-text-handlers.py 生成',
        'common': '层号 arg0 一律要求"是 int 且在 0..0x1F"，否则整条命令忽略；'
                  '其余分量逐分量 cmpb type==2 + 范围检查，不满足就跳过那一个分量'
                  '（保持原值，不是钳位）',
        'handlers': out,
        'instructions': checked,
    }
    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    print('== 已校验 %d 条关键指令、%d 个 handler ==' % (len(checked), len(out)))
    for h in out:
        print('  %-12s id=%d handler %s' % (h['name'], h['id'], h['handler']))
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        old = io.open(OUT_JSON, encoding='utf-8').read()
        if old != text:
            print('FAIL: %s 与提取结果不一致' % OUT_JSON)
            return 1
        print('OK: %s 与提取结果逐字节一致' % OUT_JSON)
        return 0
    io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n').write(text)
    print('written %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())
