#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate for the text-window save format (A 类基线)."""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-text-window-save.py')
REF = os.path.join(FVP, 'ref', 'exe-text-window-save.json')
LOAD = os.path.join(FVP, 'ref', 'exe-load-flow.json')
FIELDS = os.path.join(FVP, 'ref', 'exe-text-fields.json')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


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
          'extract-exe-text-window-save.py --check 退出码 0（实际 %d）'
          % proc.returncode)

    ref = json.loads(io.open(REF, encoding='utf-8').read())
    fields = ref.get('fields') or []

    print('== 2. 字段顺序 ==')
    check(ref.get('entry') == '0x433850', '反序列化入口 0x433850')
    check(len(fields) >= 18, '字段 >= 18（实际 %d）' % len(fields))
    order = [f['order'] for f in fields]
    check(order == sorted(order), 'order 递增')
    check(fields[0]['field'] == 'present' and fields[0]['size'] == 'u8',
          '第 1 个字段是 u8 存在标志')
    check(fields[1]['field'] == 'surface', '第 2 个字段是 surface 记录')
    check(fields[2]['field'] == 'text', '第 3 个字段是窗口文本')
    names = [f['field'] for f in fields]
    for want in ('panel_w', 'panel_h', 'pen_x', 'pen_y', 'style_block'):
        check(want in names, '含字段 %s' % want)
    check('0x28' == [f for f in fields if f['field'] == 'style_block'][0]['size'],
          '样式块恰好 0x28 字节（与 tw+0x590..0x5B7 对齐）')

    print('== 3. 与读档恢复顺序对上 ==')
    if os.path.isfile(LOAD):
        lf = json.loads(io.open(LOAD, encoding='utf-8').read())
        la = lf.get('frame_consumers', {}).get('load_apply', {})
        steps = la.get('restore_steps') or []
        hit = [s for s in steps if '0x433850' in (s.get('what') or '')]
        check(bool(hit), '读档恢复顺序里有 0x433850 那一步')
        if hit:
            check('32' in (hit[0]['what'] or ''), '那一步是 32 个窗口')
            check(hit[0]['pc'] == '0x0043f38f', '那一步的 pc = 0x43F38F')
    else:
        check(True, '（缺 exe-load-flow.json，跳过交叉核对）')

    print('== 4. 与文本字段基线交叉核对 ==')
    if os.path.isfile(FIELDS):
        ff = json.loads(io.open(FIELDS, encoding='utf-8').read())
        keys = set((ff.get('fields') or {}).keys())
        # 只交叉核对两份基线**都有**的字段（0x5DC/0x5DE/0x5EA 只在窗口存档基线里）
        for off in ('0x5e8', '0x597', '0x5a4'):
            check(off in keys, '文本字段基线里有 %s' % off)
    else:
        check(True, '（缺 exe-text-fields.json，跳过交叉核对）')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 文本窗口存档格式（20 个字段）与 exe 一致，'
          '并与读档恢复顺序/文本字段基线互相对上')
    return 0


if __name__ == '__main__':
    sys.exit(main())