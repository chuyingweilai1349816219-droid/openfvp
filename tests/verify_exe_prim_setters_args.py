#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate for ref/exe-prim-setters-args.json + the C-side PrimSetRS fix."""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-prim-setters-args.py')
REF = os.path.join(FVP, 'ref', 'exe-prim-setters-args.json')
SETTERS = os.path.join(FVP, 'ref', 'exe-prim-setters.json')
SYSCALL = os.path.join(FVP, 'src', 'syscall.c')

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
    check(proc.returncode == 0, 'extract-exe-prim-setters-args.py --check 退出码 0')

    ref = json.loads(io.open(REF, encoding='utf-8').read())
    hs = ref.get('handlers') or []

    print('== 2. 四个 handler 的规则 ==')
    check(len(hs) == 15, '十五个 handler（实际 %d）' % len(hs))
    by = {h['handler']: h for h in hs}
    for addr in ('0x43ace0', '0x43a8b0', '0x43b9c0', '0x437990', '0x4379d0',
                 '0x43ba20',
                 '0x43ba90', '0x43baf0',
                 '0x43bc40', '0x43bd00', '0x43bdc0', '0x43be40', '0x43bef0',
                 '0x43bf50', '0x43bfa0'):
        check(addr in by, '含 handler %s' % addr)
    op = by.get('0x43ba90') or {}
    check(any('0x20' in w for w in op.get('writes') or []), '0x43BA90 写 op1(+0x20)')
    rs = by.get('0x43baf0') or {}
    rules = ' '.join(rs.get('rules') or [])
    check('3600' in rules, '0x43BAF0 angle 取模 3600')
    check('100..10000' in rules, '0x43BAF0 scale 钳位 100..10000')
    check('0x40' in rules, '0x43BAF0 写分量时 flags |= 0x40')
    txt = by.get('0x43bdc0') or {}
    check(any('0x32' in w for w in txt.get('writes') or []),
          '0x43BDC0 写文本层 +0x32')
    check('0..0x1F' in ' '.join(txt.get('rules') or []),
          '0x43BDC0 层号范围 0..0x1F')
    t2 = by.get('0x43be40') or {}
    check(any('0x30' in w for w in t2.get('writes') or []),
          '0x43BE40 写 +0x30')
    wh = by.get('0x43bf50') or {}
    check(any('0x18' in w for w in wh.get('writes') or []),
          '0x43BF50 写 w(+0x18)')
    xy = by.get('0x43bfa0') or {}
    check(any('0x14' in w for w in xy.get('writes') or []),
          '0x43BFA0 写 x(+0x14)')
    uv = by.get('0x43bef0') or {}
    check(any('0x1C' in w for w in uv.get('writes') or []),
          '0x43BEF0 写 +0x1C')
    for addr, ty in (('0x43bc40', '7'), ('0x43bd00', '4')):
        h = by.get(addr) or {}
        r = ' '.join(h.get('rules') or [])
        check('0x439bf0' in r.lower() or '0x439BF0' in r, '%s 用 0x439BF0 造图元' % addr)
        check('type-%s' % ty in r or 'type %s' % ty in r, '%s 造 type-%s' % (addr, ty))
    bl = by.get('0x43ba20') or {}
    check(any('0x08' in w for w in bl.get('writes') or []),
          '0x43BA20（PrimSetBlend）写 prim+0x08')
    check('0..1' in ' '.join(bl.get('rules') or []), 'blend 范围 0..1')
    check(bl.get('confirmed_id') == 78, 'PrimSetBlend 的 id = 78（已证实）')
    al = by.get('0x437990') or {}
    check(any('0x07' in w for w in al.get('writes') or []),
          '0x437990（PrimSetAlpha）写 prim+0x07')
    check('0..0xFF' in ' '.join(al.get('rules') or []), 'alpha 范围 0..0xFF')
    dr = by.get('0x4379d0') or {}
    check(any('0x05' in w for w in dr.get('writes') or []),
          '0x4379D0（PrimSetDraw）写 prim+0x05')
    check('0 或 1' in ' '.join(dr.get('rules') or []), 'draw 只接受 0/1')
    ma = by.get('0x43a8b0') or {}
    check(any('0x42d460' in w for w in ma.get('writes') or []),
          '0x43A8B0（MotionAlpha）尾调 0x42D460')
    check('300000' in ' '.join(ma.get('rules') or []), 'ms 范围 1..300000')
    check('当前 prim+0x07' in ' '.join(ma.get('rules') or []),
          'from/to 缺失时回退成当前 alpha')
    ph = by.get('0x43b9c0') or {}
    check(any('0x814' in w for w in ph.get('writes') or []),
          '0x43B9C0（PrimHit）写线程 +0x814')
    mm = by.get('0x43ace0') or {}
    check(any('0x814' in w for w in mm.get('writes') or []),
          '0x43ACE0（MotionMoveTest）写线程 +0x814')
    check('还在跑' in ' '.join(mm.get('rules') or []), '规则写明 1 = 还在跑')
    for addr, cid in (('0x43ace0', 50), ('0x43a8b0', 42), ('0x43b9c0', 76),
                      ('0x437990', 77), ('0x4379d0', 79),
                      ('0x43ba90', 81), ('0x43bf50', 89), ('0x43bfa0', 90),
                      ('0x43bc40', 84), ('0x43bd00', 85), ('0x43bdc0', 86),
                      ('0x43be40', 87), ('0x43bef0', 88)):
        check((by.get(addr) or {}).get('confirmed_id') == cid,
              '%s 的 id = %d（已证实）' % (addr, cid))
    check(bool(ref.get('unknown')), 'unknown 里保留未解项')

    print('== 3. 与上一轮的写入点扫描交叉核对 ==')
    if os.path.isfile(SETTERS):
        st = json.loads(io.open(SETTERS, encoding='utf-8').read())
        pcs = set(s['pc'] for s in st.get('stores') or [])
        for want in ('0x43baca', '0x43bada', '0x43bb34', '0x43bb4f', '0x43bd56'):
            check(want in pcs, '扫描基线里有写入点 %s' % want)

    print('== 4. C 侧：case 82 已按 handler 语义修正 ==')
    if os.path.isfile(SYSCALL):
        src = io.open(SYSCALL, encoding='utf-8').read()
        i = src.index('case 82:')
        seg = src[i:i + 1200]
        check('% 3600' in seg, 'case 82 有 angle % 3600')
        check('ang < 0' in seg and 'ang += 3600' in seg, 'case 82 负数归一化')
        check('sc < 100' in seg and 'sc > 10000' in seg, 'case 82 scale 钳位 100..10000')
        check('FVP_S16' in src, 'C 侧有 int16 截断宏')
        k50 = src.index('case 50:')
        seg50 = src[k50:k50 + 900]
        check('gfx_motion_test' in seg50, 'case 50 用 gfx_motion_test 查询')
        check('tid >= 1 && tid <= 0xFFF' in seg50, 'case 50 校验 id 1..0xFFF')
        k42 = src.index('case 42:')
        seg42 = src[k42:k42 + 2000]
        check('ms >= 1 && ms <= 300000' in seg42, 'case 42 校验 ms 1..300000')
        check('fvp_gfx_int(&a[1])' in seg42, 'case 42 读 arg1 当起始 alpha')
        check('gfx_get_prim(g, id, GFX_F_ALPHA, &from)' in seg42,
              'case 42 回退读当前 alpha')
        k77 = src.index('case 77:')
        check('alpha >= 0 && alpha <= 0xFF' in src[k77:k77 + 900],
              'case 77 只接受 alpha 0..0xFF')
        k79 = src.index('case 79:')
        check('draw == 0 || draw == 1' in src[k79:k79 + 900],
              'case 79 只接受 draw 0/1')
        k78 = src.index('case 78:')
        seg78 = src[k78:k78 + 900]
        check('blend >= 0 && blend <= 1' in seg78, 'case 78 只接受 blend 0..1')
        for case in ('case 81:', 'case 89:', 'case 90:', 'case 88:'):
            k = src.index(case)
            seg2 = src[k:k + 900]
            check('FVP_S16' in seg2, '%s 用 int16 截断' % case)
        # 84：造 type-7 + 位置 + 0..1 开关
        k = src.index('case 84:')
        seg84 = src[src.index('case 84:', k):]
        seg84 = seg84[:1600]
        check('GFX_F_TYPE, 7' in seg84, 'case 84 造 type-7 图元')
        check('on >= 0 && on <= 1' in seg84, 'case 84 开关只接受 0..1')
        check('GFX_F_X' in seg84 and 'GFX_F_Y' in seg84, 'case 84 写 x/y')
        # 85/86/87 的范围检查与原版 handler 对齐
        k = src.index('case 85:')
        seg = src[k:k + 900]
        check('slot >= -2 && slot <= 0xFFF' in seg, 'case 85 只接受 -2..0xFFF')
        k = src.index('case 86:')
        seg = src[k:k + 1100]
        check('layer >= 0 && layer <= 0x1F' in seg, 'case 86 层号只接受 0..0x1F')
        k = src.index('case 87:')
        seg = src[k:k + 1600]
        check('tile >= -1 && tile <= 0xFF' in seg, 'case 87 tile 只接受 -1..0xFF')
        for f in ('GFX_F_X', 'GFX_F_Y', 'GFX_F_W', 'GFX_F_H'):
            check(f in seg, 'case 87 写 %s（原版 arg2..arg5）' % f)

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 十五个图元 handler 的实参规则已冻结，case 82 已按原版语义'
          '（angle mod 3600 / scale 100..10000 / int16 存储）修正')
    return 0


if __name__ == '__main__':
    sys.exit(main())