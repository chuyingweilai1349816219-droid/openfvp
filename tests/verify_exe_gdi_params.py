#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""原版 exe 的 GDI 文字绘制协议：静态提取结果的回归门禁。

为什么要有这个门禁：docs/original-verification.md 里有一类"缺口"以前被写成
"需要跑原版才能知道"。但**绘制协议**（用哪个 API、传什么参数、什么字符集、
什么位图格式）不是像素，是 exe 里的常量和调用约定——静态分析就能读出来。
读出来的东西必须冻结成基线，否则以后任何人（包括我们自己）改脚本、换 exe、
改静态分析都会悄悄把这批事实弄丢。

本门禁做三件事：
  1. 调 scripts/extract-exe-gdi.py --check，确认重新提取的结果与
     ref/exe-gdi-params.json **逐字节一致**（防止提取逻辑漂移）；
  2. 断言这批参数里的**关键事实**（不变量），它们是"和原版绘制一致"的前提：
     - CreateFontA 用 SHIFTJIS_CHARSET(0x80)、宽/倾角/字重/斜体等全为 0，
       字体族名来自数据（寄存器），不是写死在 exe 里；
     - GetGlyphOutlineA 只用 GGO_METRICS(0) 与 GGO_BITMAP(1)，不用
       GGO_NATIVE/GGO_GRAY*，即原版文字是**1bpp 位图**、不是灰度抗锯齿；
     - 三次调用共用同一个 MAT2 指针，且该矩阵是单位矩阵（16.16 定点）；
  3. 打印"原版协议 vs 我们外壳"的对照表（只报告，不断言），让差距可见。

缺原版 exe 或静态分析时打印 SKIP 并返回 0（与 verify_vs_original.py 一致）。

用法：python tests/verify_exe_gdi_params.py
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-gdi.py')
REF = os.path.join(FVP, 'ref', 'exe-gdi-params.json')
EXE = os.environ.get('FVP_EXE', os.path.join(
    r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD', 'iroseka_HD.exe'))
ASM = os.path.join(FVP, 'work', 'iroseka_HD.asm')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def run(argv, timeout=600):
    proc = subprocess.run(argv, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout)
    return proc.returncode, proc.stdout.decode('utf-8', 'replace')


def imm(arg):
    """{'kind':'imm','value':N} → N；其它 → None"""
    if isinstance(arg, dict) and arg.get('kind') == 'imm':
        return arg.get('value')
    return None


def main():
    if not os.path.isfile(EXE) or not os.path.isfile(ASM):
        print('SKIP: 缺原版 exe 或静态分析（%s / %s）' % (EXE, ASM))
        print('SKIP: 这是可选语料，见 docs/original-verification.md §8')
        return 0
    if not os.path.isfile(REF):
        print('FAIL: 缺基线 %s' % REF)
        return 1

    print('== 1. 重新提取并与基线逐字节比对 ==')
    code, out = run([PY, EXTRACT, '--check'])
    print(out.rstrip())
    check(code == 0, 'extract-exe-gdi.py --check 退出码 0（实际 %d）' % code)

    with open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)

    print('== 2. 不变量：原版绘制协议 ==')
    inv = ref.get('invariants') or {}
    check(inv.get('charset_value') == 0x80,
          'CreateFontA 的 fdwCharSet == SHIFTJIS_CHARSET(0x80)（实际 %r）'
          % (inv.get('charset_value'),))
    check(inv.get('charset_is_shiftjis') is True, 'charset_is_shiftjis')
    check(inv.get('glyph_sites') == 3, 'GetGlyphOutlineA 调用点 3 个（实际 %r）'
          % (inv.get('glyph_sites'),))
    check(inv.get('glyph_uformats') == [0, 1],
          'GetGlyphOutlineA 只用 GGO_METRICS(0) 与 GGO_BITMAP(1)（实际 %r）'
          % (inv.get('glyph_uformats'),))
    check(inv.get('mat2_all_same') is True, '三次 GetGlyphOutlineA 共用同一个 MAT2')

    mats = [v for v in (ref.get('static_data') or {}).values()
            if v.get('kind') == 'MAT2']
    check(len(mats) == 1, '静态数据里恰有 1 个 MAT2（实际 %d）' % len(mats))
    if mats:
        check(mats[0]['fixed16_16'] == ['1.0000', '0.0000',
                                       '0.0000', '1.0000'],
              'MAT2 是单位矩阵（实际 %r）' % (mats[0]['fixed16_16'],))

    print('== 2.5 内置字体注册表（ShiftJIS 字体名表） ==')
    names = inv.get('font_names') or []
    want = ['ＭＳ ゴシック', 'ＭＳ 明朝', 'ＭＳ Ｐゴシック', 'ＭＳ Ｐ明朝']
    check(names == sorted(want),
          '字体名表恰好是 4 个 ShiftJIS 族名（实际 %r）' % (names,))
    check(inv.get('font_registry_tables') == 2,
          'exe 里有 2 份相同的字体名指针表（实际 %r）'
          % (inv.get('font_registry_tables'),))
    tabs = ref.get('font_registry') or []
    check(len(tabs) == 2, 'font_registry 记录了 2 张表（实际 %d）' % len(tabs))
    for t in tabs:
        stub = [e['name'] for e in t['entries']]
        check(stub == want, '表 %s 的 4 项顺序固定（实际 %r）'
              % (t['table_va'], stub))

    print('== 3. CreateFontA 的 14 个实参 ==')
    cf = (ref.get('calls') or {}).get('CreateFontA') or []
    check(len(cf) == 1, 'CreateFontA 调用点只有 1 个（实际 %d）' % len(cf))
    if cf:
        named = cf[0].get('named') or {}
        zero_keys = ['nWidth', 'nEscapement', 'nOrientation', 'fnWeight',
                     'fdwItalic', 'fdwUnderline', 'fdwStrikeOut',
                     'fdwOutputPrecision', 'fdwClipPrecision', 'fdwQuality',
                     'fdwPitchAndFamily']
        for k in zero_keys:
            check(imm(named.get(k)) == 0, '%s == 0（实际 %r）' % (k, named.get(k)))
        check(named.get('lpszFace', {}).get('kind') == 'reg',
              'lpszFace 来自寄存器（字体族名是数据驱动，不写死在 exe 里）')
        check(len(cf[0].get('args') or []) == 14, '实参个数 14')

    print('== 4. 原版协议 vs 本仓库外壳（仅报告） ==')
    ours = {
        'charset': 'DEFAULT_CHARSET',
        'glyph_api': 'TextOutW + 32bpp DIB（读 B 通道当覆盖率）',
        'quality': 'ANTIALIASED_QUALITY',
        'face': 'Microsoft YaHei（中文回退用）',
    }
    print('  原版  charset=SHIFTJIS(0x80) / GetGlyphOutlineA GGO_BITMAP(1bpp) / '
          'quality=0 / face=数据驱动')
    print('  我们  charset=%s / %s / quality=%s / face=%s'
          % (ours['charset'], ours['glyph_api'], ours['quality'], ours['face']))
    print('  说明：这条差异是"像素级一致"的主要剩余原因，见 '
          'docs/original-verification.md §8.4')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 原版绘制协议基线与提取结果一致')
    return 0


if __name__ == '__main__':
    sys.exit(main())