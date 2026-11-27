#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""正文构建器调用站点的门禁（`ref/text-builder-calls.json`）。

正文构建器 `0x3A11F`（`initstack 4|0`）只能经转发器 `0x3A2EE` 进入，脚本侧的每个
调用点都是 `pushstring <正文>; pushfalse ×3; call 0x3A2EE` —— 也就是 **arg1 是正文、
arg2..arg4 是三个 op-0x08 哨兵**。这条事实是上一轮用 `FVP_DBG_CALL=1` 看实参得出的，
这条门禁负责把"到底有多少个调用点、分布在哪"钉住（静态扫字节会命中数据段里的同形
序列，5 万条假阳性，所以只能取运行时日志）。
"""
import argparse
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
REF = os.path.join(FVP, 'ref', 'text-builder-calls.json')
EXTRACTOR = os.path.join(FVP, 'scripts', 'scan-text-builder-calls.py')


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
        print('verify_text_builder_calls: FAIL missing %s' % a.json)
        return 1
    with open(a.json, encoding='utf-8') as f:
        ref = json.load(f)

    print('== 冻结的调用站点')
    check(ref.get('schema') == 'fvp-text-builder-calls/1',
          'schema fvp-text-builder-calls/1')
    check(ref.get('builder') == '0x3a11f' and ref.get('forwarder') == '0x3a2ee',
          '构建器 0x3A11F / 转发器 0x3A2EE')
    check(ref.get('argc') == 4, '构建器收 4 个实参')
    sites = ref.get('sites') or []
    check(len(sites) == ref.get('site_count'),
          '站点数与 site_count 自洽（%d）' % len(sites))
    check(ref.get('site_count') == 133,
          '脚本站点 133 个（实际 %s）' % ref.get('site_count'))
    check(sites == sorted(sites), '站点按地址升序')
    check(ref.get('range') == [sites[0], sites[-1]] if sites else False,
          'range 与首尾一致')
    check(all(s.startswith('0x7e') or s.startswith('0x7f') or
              s.startswith('0x80') for s in sites),
          '全部落在序章代码区 0x7E8xx..0x809xx')
    check(ref.get('forwarder_calls') == 133,
          '转发器内部调用次数 = 站点数（每个站点各调一次）')

    print('== 重新跑一遍运行时日志')
    mod = load_module('scan_text_builder_calls', EXTRACTOR)
    got = mod.build()
    if got is None:
        print('  skip  bin/fvm.exe 或 iroseka.hcb 不存在')
    else:
        check(got.get('sites') == sites, '站点列表逐项相等')
        check(got.get('total_calls') == ref.get('total_calls'),
              '总调用次数相等（%s）' % ref.get('total_calls'))

    if fails:
        print('verify_text_builder_calls: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_text_builder_calls: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())