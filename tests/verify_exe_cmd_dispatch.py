#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate for ref/exe-cmd-dispatch.json（命令 id → handler），并与其它基线互证。"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-cmd-dispatch.py')
REF = os.path.join(FVP, 'ref', 'exe-cmd-dispatch.json')
TEXT_HANDLERS = os.path.join(FVP, 'ref', 'exe-text-handlers.json')
SETTERS = os.path.join(FVP, 'ref', 'exe-prim-setters-args.json')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def main():
    exe = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                       'iroseka_HD.exe')
    if not os.path.isfile(exe):
        print('SKIP: %s 不存在' % exe)
        return 0
    if not os.path.isfile(REF):
        print('FAIL: 缺基线 %s' % REF)
        return 1

    print('== 1. 重新提取并与基线逐字节比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', EXTRACT, '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=600)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0, 'extract-exe-cmd-dispatch.py --check 退出码 0')

    ref = json.loads(io.open(REF, encoding='utf-8').read())
    cmds = {c['id']: c for c in ref.get('commands') or []}

    print('== 2. 覆盖率 ==')
    check(ref.get('joined', 0) >= 130, '至少 130 条命令连上 id（实际 %d）'
          % ref.get('joined', 0))
    check(len(ref.get('missing') or []) <= 20, '未连上的 <= 20（实际 %d）'
          % len(ref.get('missing') or []))
    for cid in (78, 81, 89, 90, 110, 141):
        check(cid in cmds, '含命令 %d（%s）' % (cid, cmds.get(cid, {}).get('name')))
    for c in cmds.values():
        h = int(c['handler'], 16)
        check(0x401000 <= h < 0x492000, 'cmd %d 的 handler 在 .text 里（%s）'
              % (c['id'], c['handler']))

    print('== 3. 与文本 handler 基线互证（cmd 110/112/126 等）==')
    if os.path.isfile(TEXT_HANDLERS):
        th = json.loads(io.open(TEXT_HANDLERS, encoding='utf-8').read())
        hs = th.get('handlers')
        by_id = {}
        if isinstance(hs, list):
            for h in hs:
                by_id[int(h.get('id'))] = h
        elif isinstance(hs, dict):
            for k, v in hs.items():
                by_id[int(k)] = v
        for cid, want in ((110, '0x43c830'), (112, '0x43c8d0'),
                          (120, '0x43c960'), (126, '0x43cd60')):
            got = (cmds.get(cid) or {}).get('handler')
            check(got == want, 'cmd %d → %s（文本基线里也是 %s）'
                  % (cid, got, want))
            entry = by_id.get(cid) or {}
            if entry:
                check(entry.get('handler') == want,
                      '文本 handler 基线 cmd %d = %s' % (cid, want))

    print('== 4. 与图元 setter 基线互证（78/81/89/90）==')
    if os.path.isfile(SETTERS):
        st = json.loads(io.open(SETTERS, encoding='utf-8').read())
        handlers = set(h['handler'] for h in st.get('handlers') or [])
        for cid in (81, 89, 90):
            got = (cmds.get(cid) or {}).get('handler')
            check(got in handlers, 'cmd %d 的 handler %s 在 setter 基线里'
                  % (cid, got))
        check((cmds.get(78) or {}).get('handler') == '0x43ba20',
              'cmd 78（PrimSetBlend）→ 0x43BA20（还没精读的 handler）')
        check('0x43ba20' not in handlers,
              '0x43BA20 尚未进 setter 基线（下一轮读它）')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 命令 id → handler 表已冻结（%d/%d），并与文本/图元两条 handler 基线互证'
          % (ref.get('joined'), ref.get('joined') + len(ref.get('missing') or [])))
    return 0


if __name__ == '__main__':
    sys.exit(main())