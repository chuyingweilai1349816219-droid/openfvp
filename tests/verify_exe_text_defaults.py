#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""文本窗口**默认值**的门禁：C 里的默认值必须等于 exe 构造函数写的常量。

上一轮发现样式字段的写点里有一处 0x430EA0 的函数；读透后它是**文本窗口构造函数**，
把整套样式字段初始化成引擎默认值。我们之前按文档抄的是"12x12、字体号 0"，
与 exe 不一致——这正是"操作上不一样"的那类偏差。本轮：
`scripts/extract-exe-text-defaults.py` → `ref/exe-text-defaults.json` → 本门禁。

断言（全部来自静态分析，不猜）：

  * 构造函数 `0x430EA0` 写的默认值：字号 `+0x592=16`（宽）/ `+0x594=12`（高）；
    字体号 `+0x590=0xFFFE`（word，= **-2**）/ `+0x593=0xFE`（byte，= **-2**）；
    `+0x5A8/+0x5AA/+0x5AC/+0x5AE/+0x5B0/+0x5B4/+0x597/+0x5A4/+0x5A5/+0x5A6 = 0`；
    `+0x5D4=1`、`+0x5D9=1`、`+0x5E0=0`；
  * 尺寸上限 `0x400 = 1024`（构造函数的两个 `cmp $0x400`，错误分支里也打印 1024）；
  * 清空例程：`+0x5E8 := +0x5AC`（行宽累计从"下限/初始宽度"开始）、
    `+0x5EA := 0`、`+0x5D8 := 0`、`+0x5D6 := 0`、`+0x640 := 0`；
  * **交叉验证**：`src/textattr.c` 里 `fvp_textattr_reset()` 的默认值必须与上面一致
    （宽 16、高 12、字体号 -2/-2、间距/阴影/skip 全 0）。

缺原版程序清单时 SKIP 并返回 0。

用法：python tests/verify_exe_text_defaults.py
"""
import io
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-text-defaults.py')
REF = os.path.join(FVP, 'ref', 'exe-text-defaults.json')
TEXTATTR = os.path.join(FVP, 'src', 'textattr.c')
ASM = os.path.join(FVP, 'work', 'iroseka_HD.asm')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def signed(v, size):
    bits = size * 8
    v &= (1 << bits) - 1
    if v >= (1 << (bits - 1)):
        v -= (1 << bits)
    return v


def defaults_of(ref):
    out = {}
    for key, e in (ref.get('constructor') or {}).get('defaults', {}).items():
        if e.get('kind') != 'imm':
            continue
        out[e['offset']] = signed(e['value'], e['size'])
    return out


def reset_values(src):
    """从 fvp_textattr_reset() 里解析 `attrs[i].FIELD = VALUE;`。"""
    i = src.find('void fvp_textattr_reset')
    j = src.find('\n}', i)
    body = src[i:j if j > i else i + 4000]
    out = {}
    for m in re.finditer(r'attrs\[i\]\.(\w+)\s*=\s*(-?\w+);', body):
        try:
            out[m.group(1)] = int(m.group(2), 0)
        except ValueError:
            pass
    return out


def main():
    if not os.path.isfile(ASM):
        print('SKIP: 缺原版程序清单 %s（work/ 不入库）' % ASM)
        return 0
    if not os.path.isfile(REF):
        print('FAIL: 缺基线 %s' % REF)
        return 1

    print('== 1. 重新提取并与基线逐字节比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', EXTRACT, '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=600)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0,
          'extract-exe-text-defaults.py --check 退出码 0（实际 %d）'
          % proc.returncode)

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)
    d = defaults_of(ref)
    ctor = ref.get('constructor') or {}

    print('== 2. 构造函数 0x430EA0 的默认值（按写入宽度取有符号值）==')
    check(ctor.get('start') == '0x430ea0',
          '构造函数起点 0x430EA0（实际 %r）' % ctor.get('start'))
    check(d.get(0x592) == 16, '默认字号宽 +0x592 = 16（实际 %r）' % d.get(0x592))
    check(d.get(0x594) == 12, '默认字号高 +0x594 = 12（实际 %r）' % d.get(0x594))
    check(d.get(0x590) == -2,
          '默认字体号 +0x590 = 0xFFFE（word，-2；实际 %r）' % d.get(0x590))
    check(d.get(0x593) == -2,
          '默认字体号2 +0x593 = 0xFE（byte，-2；实际 %r）' % d.get(0x593))
    for off, name in ((0x5A4, 'TextFunction arg2'), (0x5A5, 'TextFunction arg1'),
                      (0x5A6, 'TextFunction arg3'), (0x5A8, 'space_x'),
                      (0x5AA, 'space_y'), (0x5AC, 'fmt_pad_b'),
                      (0x5AE, 'fmt_pad_a'), (0x5B0, 'fmt_off_b'),
                      (0x5B4, 'fmt_off_a'), (0x597, 'shadow_dist'),
                      (0x5E0, 'speed_ms')):
        check(d.get(off) == 0, '默认 %s（+0x%x）= 0（实际 %r）'
              % (name, off, d.get(off)))
    check(d.get(0x5D4) == 1, '构造时 +0x5D4 = 1（实际 %r）' % d.get(0x5D4))
    check(d.get(0x5D9) == 1, '构造时 +0x5D9 = 1（实际 %r）' % d.get(0x5D9))

    print('== 3. 尺寸上限 0x400 = 1024 ==')
    check(ctor.get('size_limits') == [1024],
          '构造函数里的尺寸上限只有 1024（实际 %r）'
          % (ctor.get('size_limits'),))

    print('== 4. 清空例程：行宽累计从 +0x5AC 起算 ==')
    resets = {r['field']: r for r in (ref.get('clear_routine') or {}).get(
        'resets', [])}
    e = resets.get(0x5E8) or {}
    v = e.get('value')
    check(isinstance(v, list) and v and v[0] == 'load' and v[1] == 0x5AC,
          '+0x5E8 := +0x5AC（从初始宽度/下限起算，实际 %r）' % (v,))
    for off in (0x5EA, 0x5D8, 0x5D6, 0x640):
        e = resets.get(off) or {}
        v = e.get('value')
        check(isinstance(v, list) and v and v[0] == 'imm' and v[1] == 0,
              '清空时 +0x%x := 0（实际 %r）' % (off, v))

    print('== 5. 交叉验证：src/textattr.c 的默认值 ==')
    if not os.path.isfile(TEXTATTR):
        check(False, '缺 %s' % TEXTATTR)
    else:
        src = io.open(TEXTATTR, encoding='utf-8').read()
        rv = reset_values(src)
        check(rv.get('w') == d.get(0x592),
              '宽度默认值 == 16（代码 %r / exe %r）'
              % (rv.get('w'), d.get(0x592)))
        check(rv.get('h') == d.get(0x594),
              '高度默认值 == 12（代码 %r / exe %r）'
              % (rv.get('h'), d.get(0x594)))
        check(rv.get('font_id') == d.get(0x590),
              'font_id 默认值 == -2（代码 %r / exe %r）'
              % (rv.get('font_id'), d.get(0x590)))
        check(rv.get('font_id2') == d.get(0x593),
              'font_id2 默认值 == -2（代码 %r / exe %r）'
              % (rv.get('font_id2'), d.get(0x593)))
        for field in ('space_x', 'space_y', 'shadow_dist', 'skip_mode',
                      'speed_ms', 'fmt_pad_a', 'fmt_pad_b', 'fmt_off_a',
                      'fmt_off_b', 'func_a', 'func_b', 'func_c',
                      'fmt_width_floor'):
            check(rv.get(field) == 0, '默认 %s = 0（实际 %r）'
                  % (field, rv.get(field)))
        check(rv.get('r') == 255 and rv.get('g') == 255 and rv.get('b') == 255,
              '默认颜色白字 255/255/255')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 文本窗口默认值（16x12、字体号 -2/-2、间距 0、上限 1024、'
          '行宽累计从 +0x5AC 起算）与 exe 构造函数一致')
    return 0


if __name__ == '__main__':
    sys.exit(main())