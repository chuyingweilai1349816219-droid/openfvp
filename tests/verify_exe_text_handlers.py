#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""文本类 handler 实参规则的门禁（A 类）。

`scripts/extract-exe-text-handlers.py` 把四个文本 handler 的实参规则抄成
`ref/exe-text-handlers.json`。本门禁断言：

  1. 重新提取 == 基线（逐字节）；
  2. 四个 handler：TextBuff(110)/TextColor(112)/TextOutSize(120)/TextSize(126)，
     各自 handler 地址与关键指令有记录；
  3. 共同规则：层号 arg0 必须是 int 且在 0..0x1F；其余分量逐分量判 int + 范围，
     不满足**保持原值**（不是钳位）；
  4. 范围：TextColor 0..0xFF；TextOutSize 0..0xC / 0..0x8；
     TextSize 0xC..0x40 / 0x8..0x20；TextBuff w/h 缺省 8；
  5. C 侧一致性：`gfx_text_set_color_ex()` 逐分量判 0..0xFF、不满足保留原值；
     `syscall.c` 的 case 112 调它（不是无条件 `gfx_text_set_color`）。

缺 `work/iroseka_HD.asm` 时 SKIP 并返回 0。

用法：python tests/verify_exe_text_handlers.py
"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-text-handlers.py')
REF = os.path.join(FVP, 'ref', 'exe-text-handlers.json')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def read(path):
    return io.open(path, encoding='utf-8').read()


def main():
    asm = os.path.join(FVP, 'work', 'iroseka_HD.asm')
    if not os.path.isfile(asm):
        print('SKIP: 缺原版程序清单 %s（work/ 不入库）' % asm)
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
          'extract-exe-text-handlers.py --check 退出码 0（实际 %d）'
          % proc.returncode)

    ref = json.loads(read(REF))
    hs = {h['id']: h for h in ref.get('handlers') or []}

    print('== 2. 四个 handler ==')
    want = {110: ('TextBuff', '0x43c830'), 112: ('TextColor', '0x43c8d0'),
            113: ('TextFont', '0x43da70'), 120: ('TextOutSize', '0x43c960'),
            125: ('TextShadowDist', '0x43cbb0'), 126: ('TextSize', '0x43cd60'),
            127: ('TextSkip', '0x43cdd0'), 128: ('TextSpace', '0x43ce10')}
    check(set(hs) == set(want), 'handler 集合 = %r（实际 %r）'
          % (sorted(want), sorted(hs)))
    for cid, (name, pc) in want.items():
        h = hs.get(cid) or {}
        check(h.get('name') == name and h.get('handler') == pc,
              '%s id=%d handler %s' % (name, cid, pc))

    print('== 3. 共同规则 ==')
    common = ref.get('common') or ''
    check('0..0x1F' in common, '共同规则写明层号 0..0x1F')
    check('保持原值' in common and '不是钳位' in common,
          '共同规则写明"不满足保持原值、不是钳位"')

    print('== 4. 各 handler 的范围 ==')
    r112 = (hs.get(112) or {}).get('rules') or {}
    check('0xFF' in (r112.get('r') or '') and '保持原值' in (r112.get('r') or ''),
          'TextColor 分量 0..0xFF 且不满足保持原值')
    r120 = (hs.get(120) or {}).get('rules') or {}
    check('0xC' in (r120.get('a') or '') and '0x8' in (r120.get('b') or ''),
          'TextOutSize 0..0xC / 0..0x8')
    r126 = (hs.get(126) or {}).get('rules') or {}
    check('0xC..0x40' in (r126.get('w') or '') and '0x8..0x20' in (r126.get('h') or ''),
          'TextSize 0xC..0x40 / 0x8..0x20')
    r110 = (hs.get(110) or {}).get('rules') or {}
    check('缺省 8' in (r110.get('w') or '') and '缺省 8' in (r110.get('h') or ''),
          'TextBuff w/h 缺省 8')
    r113 = (hs.get(113) or {}).get('rules') or {}
    check('动态' in (r113.get('out_of_range') or ''),
          'TextFont 的上界是动态的（字体表项数-1）')
    r125 = (hs.get(125) or {}).get('rules') or {}
    check('钳位' in (r125.get('dist') or ''),
          'TextShadowDist 是**钳位** 0..0xC（与其他 handler 的"跳过"不同）')
    r127 = (hs.get(127) or {}).get('rules') or {}
    check('0..3' in (r127.get('mode') or ''), 'TextSkip mode 0..3')
    r128 = (hs.get(128) or {}).get('rules') or {}
    check('-0x20..0x20' in (r128.get('x') or ''), 'TextSpace x -0x20..0x20')

    print('== 5. C 侧一致性 ==')
    gfx = read(os.path.join(FVP, 'src', 'gfx.c'))
    i = gfx.index('void gfx_text_set_color_ex')
    seg = gfx[i:i + 900]
    check('r >= 0 && r <= 0xFF' in seg and 'gr >= 0 && gr <= 0xFF' in seg and
          'b >= 0 && b <= 0xFF' in seg,
          'gfx_text_set_color_ex() 逐分量判 0..0xFF')
    check('has_r &&' in seg and 'has_g &&' in seg and 'has_b &&' in seg,
          '非 int 的分量被跳过')
    sysc = read(os.path.join(FVP, 'src', 'syscall.c'))
    seg2 = sysc[sysc.index('case 112:'):sysc.index('case 120:')]
    check('gfx_text_set_color_ex' in seg2, 'syscall case 112 用逐分量入口')
    check('fvp_gfx_is_int(&a[1])' in seg2 and 'fvp_gfx_is_int(&a[2])' in seg2 and
          'fvp_gfx_is_int(&a[3])' in seg2,
          'case 112 逐个判断参数是不是 int')
    seg3 = sysc[sysc.index('case 113:'):sysc.index('case 125:')]
    check('fvp_vm_font_count(vm) - 1' in seg3,
          'case 113 用"字体表项数-1"当动态上界')
    check('fvp_textattr_set_font_ex' in seg3, 'case 113 用逐分量入口')
    check('has1' in seg3 and 'has2' in seg3,
          'case 113 记下每个 id 是不是 int（非 int 不覆盖）')
    ta = read(os.path.join(FVP, 'src', 'textattr.c'))
    i3 = ta.index('int fvp_textattr_set_font_ex')
    seg4 = ta[i3:i3 + 700]
    check('id >= FVP_TEXT_FONT_MIN && id <= id_max' in seg4,
          'set_font_ex 用传入的动态上界')
    check('has_id &&' in seg4 and 'has_id2 &&' in seg4, '非 int 的 id 被跳过')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 八个文本 handler 的实参规则（层号 0..0x1F；分量逐分量判 int + 范围，'
          '不满足保持原值 / TextShadowDist 钳位）与 exe 一致')
    return 0


if __name__ == '__main__':
    sys.exit(main())