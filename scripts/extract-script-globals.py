#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从两作脚本里静态提取**标题/开场相关全局变量**的读写点（回答"global3 是什么"）。

背景：标题进场与布局里有几条分支依赖脚本全局变量，我们之前只写了"语义未定"：

  * `global3 == 1` 时：体验版标记 `title_trial`(277) 显示、菜单 1522/1529（星空是
    266/274 那组）整体上移 48 px、以及 1522/1529 的 `MotionMove` 改走 −81 px 分支；
  * `global236`（五彩）/ `global232`（星空）是**跳过动画**开关，标题进场里大量分支
    读它。

本脚本只抄事实：每个 global 的 `popglobal` 写点（含紧邻的字面量）与 `pushglobal`
读点，再按命名域归类（标题场景 0x58xxx / 0x6Fxxx、开场 0x580xx..、其它）。
产物 `ref/script-globals.json`。门禁 `tests/verify_script_globals.py` 会重新提取比
对，并断言"g3 只有一个写点且写 0"这个关键结论。

用法：
    python scripts/extract-script-globals.py            # 打印 + 写 json
    python scripts/extract-script-globals.py --check    # 只比对（门禁用）
"""
import argparse
import collections
import hashlib
import io
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'ref'))
import fvm_ref  # noqa: E402

GAMES = {
    'iroseka': {
        'hcb': os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                            'iroseka.hcb'),
        'title_region': [0x58000, 0x5B000],
        'skip_global': 236,
        'trial_global': 3,
    },
    'hoshimemo': {
        'hcb': os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed',
                            'Hoshimemo_HD.hcb'),
        'title_region': [0x6F000, 0x73000],
        'skip_global': 232,
        'trial_global': 3,
    },
}
OUT_JSON = os.path.join(ROOT, 'ref', 'script-globals.json')
LIT_OPS = ('pushint8', 'pushint16', 'pushint32', 'pushfloat', 'pushstring',
           'pushtrue', 'pushfalse')


def classify(cfg, pc):
    lo, hi = cfg['title_region']
    if lo <= pc <= hi:
        return 'title'
    return 'other'


def scan(path, cfg):
    data = open(path, 'rb').read()
    code = fvm_ref.Code(data, 'cp932')
    order = sorted(code.instr)
    writes = collections.defaultdict(list)
    reads = collections.defaultdict(list)
    for i, pc in enumerate(order):
        op, nm, args = code.instr[pc]
        if nm == 'popglobal':
            val = None
            if i > 0:
                prev_pc = order[i - 1]
                pnm = code.instr[prev_pc]
                if pnm[1] in LIT_OPS:
                    if pnm[1] in ('pushtrue', 'pushfalse'):
                        val = (pnm[1] == 'pushtrue')
                    else:
                        val = pnm[2][0] if pnm[2] else None
            writes[args[0]].append({
                'pc': '0x%08x' % pc, 'value': val,
                'value_literal': code.instr[order[i - 1]][1] if i > 0 else None,
                'region': classify(cfg, pc)})
        elif nm == 'pushglobal':
            reads[args[0]].append({
                'pc': '0x%08x' % pc, 'region': classify(cfg, pc)})
    return data, writes, reads


def summarize(gid, writes, reads, cfg):
    w = writes.get(gid, [])
    r = reads.get(gid, [])
    return {
        'global': gid,
        'writes': w,
        'write_count': len(w),
        'read_count': len(r),
        'reads_in_title': sum(1 for x in r if x['region'] == 'title'),
        'first_reads': r[:8],
        'roles': [],
    }


def extract(name, cfg):
    data, writes, reads = scan(cfg['hcb'], cfg)
    trial = summarize(cfg['trial_global'], writes, reads, cfg)
    skip = summarize(cfg['skip_global'], writes, reads, cfg)

    trial_vals = sorted({str(x['value']) for x in trial['writes']})
    trial['roles'] = [
        '体验版/正式版布局开关：==1 时显示 title_trial(277)、菜单整体上移 48 px、'
        '1522/1529 的 MotionMove 走 −81 px 分支',
        '零售脚本里写点唯一且值固定（%s）' % ','.join(trial_vals),
    ]
    if trial['write_count'] == 1 and trial['writes'][0]['value'] == 0:
        trial['conclusion'] = ('唯一的写入把 g%d 写成 0，脚本里没有任何路径把它置 1；'
                               '所以"体验版布局"分支在零售脚本里不可达'
                               '（引擎是否从配置/存档写它未确认，原版程序里没找到写点）'
                               % cfg['trial_global'])
    else:
        trial['conclusion'] = '存在把 %d 置非 0 的写点，分支可能可达' \
                              % cfg['trial_global']

    skip_vals = sorted({str(x['value']) for x in skip['writes']})
    skip['roles'] = [
        '跳过动画开关：标题进场/开场大量分支读它（==1 时直接到终态）',
        '脚本里既有置 1 也有置 0 的写点（%s）' % ','.join(skip_vals),
    ]

    # 顺带统计：所有 global 的写点数量 top（帮助以后找开关）
    top = sorted(((g, len(w)) for g, w in writes.items()),
                 key=lambda kv: -kv[1])[:10]

    return {
        'hcb_sha256': hashlib.sha256(data).hexdigest(),
        'hcb_size': len(data),
        'title_region': ['0x%08x' % cfg['title_region'][0],
                         '0x%08x' % cfg['title_region'][1]],
        'trial_layout': trial,
        'skip_animation': skip,
        'most_written_globals': [{'global': g, 'writes': n} for g, n in top],
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default='all', choices=['all'] + sorted(GAMES))
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)

    result = {
        'note': '标题/开场相关脚本全局变量的读写点（global3 = 体验版布局开关，'
                'global236/232 = 跳过动画开关）；由 scripts/extract-script-globals.py '
                '生成，只含 pc 与字面量，不含游戏数据本体',
        'games': {},
    }
    for name in (sorted(GAMES) if opt.game == 'all' else [opt.game]):
        cfg = GAMES[name]
        if not os.path.isfile(cfg['hcb']):
            print('SKIP: %s 不存在' % cfg['hcb'])
            return 0
        r = extract(name, cfg)
        result['games'][name] = r
        t = r['trial_layout']
        s = r['skip_animation']
        print('%s: g%d writes=%d reads=%d | g%d writes=%d reads=%d'
              % (name, t['global'], t['write_count'], t['read_count'],
                 s['global'], s['write_count'], s['read_count']))
        print('  trial: %s' % t['conclusion'])
        print('  skip : %s' % ','.join(s['roles']))

    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        old = io.open(OUT_JSON, encoding='utf-8').read()
        if old != text:
            print('FAIL: 提取结果与 ref/script-globals.json 不一致')
            import difflib
            for line in list(difflib.unified_diff(
                    old.splitlines(), text.splitlines(), 'ref', 'new',
                    lineterm=''))[:40]:
                print(line)
            return 1
        print('OK: 与 ref/script-globals.json 一致')
        return 0
    io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n').write(text)
    print('写入 %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())