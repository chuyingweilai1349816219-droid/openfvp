#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""run_all.py -- one-shot robustness suite for the FVP engine.

Runs, in order:

  1. allocator-selfcheck  proves the fuzz-only OOB detector actually fires
                          (guard build faults, canary build exits 72)
  2. compare.py --run     existing acceptance: byte-identical text vs the
                          frozen Python reference for iroseka.hcb (900k steps)
  3. verify_assets.py --quick
                          existing asset acceptance (BIN/HZC1/INFLATE/HZC1
                          decode against the Python reference)
  4. test_snd (ogg/wav)   existing audio acceptance on the real corpus
  5. test_vm_edge.py      VM boundary conditions (new)
  6. fuzz_hcb.py          malformed .hcb mutation fuzz, in-process + fvm.exe
                          (new, >= 20,000 mutations by default)
  7. fuzz_assets.py       malformed BIN/HZC1/inflate/WAV/Ogg fuzz (new)

Nothing under fvp/src, fvp/tests/*.c (existing) or the Makefile is modified:
the new harnesses are built into fvp/out/fuzz/ from read-only sources, and the
allowed (read-only) existing tests are invoked as-is.

Usage:
    python fvp/tests/run_all.py [--fast] [--only NAMES] [--exclude NAMES]
                               [--list] [--keep] [--json PATH]

Exit code 0 = every non-skipped step passed.
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
OUT = os.path.join(FVP, 'out', 'fuzz')
TMP = os.path.join(OUT, 'tmp')

sys.path.insert(0, HERE)
import harness_build as hb  # noqa: E402
import fuzz_assets as fa  # noqa: E402
import fuzz_hcb as fh  # noqa: E402

PY = sys.executable


def log(msg):
    print(msg)
    sys.stdout.flush()


def tail(text, lines=25):
    rows = text.strip().splitlines()
    return '\n'.join(rows[-lines:])


def has_module(name):
    proc = subprocess.run([PY, '-c', 'import %s' % name],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return proc.returncode == 0


def note_from(output, keys):
    """Pick the most informative summary line, in key priority order."""
    lines = [ln.strip() for ln in output.splitlines() if ln.strip()]
    for key in keys:
        for line in lines:
            if key in line and not line.startswith('base '):
                return line[:120]
    return ''


class Step(object):
    def __init__(self, name, argv, timeout, parsers=(), skip_reason=None,
                 note=''):
        self.name = name
        self.argv = argv
        self.timeout = timeout
        self.parsers = parsers
        self.skip_reason = skip_reason
        self.note = note


def build_steps(args):
    tools = hb.public_tools()
    fvm = tools['fvm']
    test_snd = tools['test_snd']
    game = fa.find_game_dir()
    script = fh.find_script()
    steps = []

    # 1. allocator self-check (built + run by the fuzz scripts too, but listed
    #    explicitly so "0 faults" is not taken on faith).
    steps.append(Step('allocator-selfcheck', None, 120,
                      note='page-guard and canary builds must both fault'))

    # 2. reference equivalence
    compare = os.path.join(HERE, 'compare.py')
    ref = os.path.join(FVP, 'ref', 'ref-900k.txt')
    if not (os.path.isfile(fvm) and os.path.isfile(compare) and
            os.path.isfile(ref) and script):
        steps.append(Step('compare.py --run', None, 0,
                          skip_reason='missing fvm.exe, ref-900k.txt or '
                                      'iroseka.hcb'))
    else:
        steps.append(Step('compare.py --run',
                          [PY, compare, '--run', '--quiet'], 900,
                          note='byte-identical vs Python reference'))

    # 2c. 多游戏兼容性门禁（第二部作品也要能跑、可重复、编码正确）
    vmulti = os.path.join(HERE, 'verify_multigame.py')
    if not (os.path.isfile(fvm) and os.path.isfile(vmulti) and script):
        steps.append(Step('verify_multigame.py', None, 0,
                          skip_reason='missing fvm.exe or iroseka.hcb'))
    else:
        vt_multi = [PY, vmulti]
        if args.fast:
            vt_multi += ['--steps', '300000']
        steps.append(Step('verify_multigame.py', vt_multi, 900,
                          note='two FVP titles: decode, run twice, deterministic, '
                               'JP text ratio'))

    # 2b. coroutine slots must stay behaviour-neutral
    vthreads = os.path.join(HERE, 'verify_threads.py')
    if not (os.path.isfile(fvm) and os.path.isfile(vthreads) and
            os.path.isfile(ref) and script):
        steps.append(Step('verify_threads.py', None, 0,
                          skip_reason='missing fvm.exe, ref-900k.txt or '
                                      'iroseka.hcb'))
    else:
        vt_argv = [PY, vthreads]
        if args.fast:
            vt_argv.append('--fast')
        steps.append(Step('verify_threads.py',
                          vt_argv,
                          900,
                          note='ThreadStart/ThreadExit on/off both byte-exact'))

    # 3. asset acceptance
    verify = os.path.join(HERE, 'verify_assets.py')
    extract = os.path.join(FVP, 'extract', 'graph')
    if not (os.path.isfile(verify) and game and has_module('numpy')):
        steps.append(Step('verify_assets.py --quick', None, 0,
                          skip_reason='verify_assets.py, numpy or the game '
                                      'directory is unavailable'))
    else:
        steps.append(Step('verify_assets.py --quick',
                          [PY, verify, '--quick'], 3600,
                          note='BIN/HZC1/inflate vs Python reference'))

    # 4. audio acceptance (existing test)
    ogg = os.path.join(FVP, 'work', 'bgm001.ogg')
    wav = os.path.join(FVP, 'work', 'se001.wav')
    for label, path in (('test_snd (ogg)', ogg), ('test_snd (wav)', wav)):
        if not (os.path.isfile(test_snd) and os.path.isfile(path)):
            steps.append(Step(label, None, 0,
                              skip_reason='bin/test_snd.exe or %s missing' %
                                          os.path.basename(path)))
        else:
            out_wav = os.path.join(TMP, 'run_all_%s.wav' %
                                   ('ogg' if label.endswith('(ogg)') else 'wav'))
            steps.append(Step(label, [test_snd, path, out_wav], 300,
                              note='decode + mix on the real corpus'))

    # 4b. save layer: synthetic self-test, then real slots (byte-identical)
    test_save = tools['test_save']
    if not os.path.isfile(test_save):
        steps.append(Step('test_save (selftest)', None, 0,
                          skip_reason='bin/test_save.exe missing '
                                      '(run: make test_save)'))
    else:
        steps.append(Step('test_save (selftest)', [test_save], 120,
                          note='synthetic slot/save.bin round-trip + malformed '
                               'input rejection'))
        save_dir = os.path.join(game, 'save') if game else None
        sbin = os.path.join(save_dir, 'save.bin') if save_dir else None
        slots = []
        if save_dir and os.path.isdir(save_dir):
            for n in sorted(os.listdir(save_dir)):
                if (len(n) == 8 and n[0] == 's' and n[1:4].isdigit() and
                        n.endswith('.bin')):
                    slots.append(os.path.join(save_dir, n))
        if sbin and os.path.isfile(sbin) and slots:
            steps.append(Step('test_save (real slots)',
                              [test_save, '--savebin', sbin] + slots, 300,
                              note='%d slots: byte-identical re-serialisation '
                                   'against docs/save-format.md' % len(slots)))
        else:
            steps.append(Step('test_save (real slots)', None, 0,
                              skip_reason='no save/save.bin + save/sNNN.bin '
                                          'under the game directory'))

    # 4c. save.bin -> VM globals bridge
    test_save_vm = tools['test_save_vm']
    if not os.path.isfile(test_save_vm):
        steps.append(Step('test_save_vm (selftest)', None, 0,
                          skip_reason='bin/test_save_vm.exe missing '
                                      '(run: make test_save_vm)'))
    else:
        steps.append(Step('test_save_vm (selftest)', [test_save_vm], 120,
                          note='type mapping / int32 wrap / capacity / stats '
                               'only'))
        if sbin and os.path.isfile(sbin):
            steps.append(Step('test_save_vm (real save.bin)',
                              [test_save_vm, sbin], 300,
                              note='1471 = 145 int + 846 unknown + 480 unset, '
                                   'values vs docs §3.3'))
        else:
            steps.append(Step('test_save_vm (real save.bin)', None, 0,
                              skip_reason='no save/save.bin under the game '
                                          'directory'))

    # 4d. global flag bitmap (FlagGet/FlagSet storage + save.bin seeding)
    test_flags = tools['test_flags']
    if not os.path.isfile(test_flags):
        steps.append(Step('test_flags (selftest)', None, 0,
                          skip_reason='bin/test_flags.exe missing '
                                      '(run: make test_flags)'))
    else:
        steps.append(Step('test_flags (selftest)', [test_flags], 120,
                          note='2048-bit map: get/set/clear, bounds, popcount, '
                               'NULL safety'))
        if sbin and os.path.isfile(sbin):
            steps.append(Step('test_flags (real save.bin)',
                              [test_flags, sbin], 300,
                              note='save.bin flag bitmap equals the seeded map '
                                   '(docs §3.5: 0 bits in this title)'))
        else:
            steps.append(Step('test_flags (real save.bin)', None, 0,
                              skip_reason='no save/save.bin under the game '
                                          'directory'))

    # 4e. text window attributes (TextColor/TextSize/TextSpeed storage)
    test_textattr = tools['test_textattr']
    if not os.path.isfile(test_textattr):
        steps.append(Step('test_textattr', None, 0,
                          skip_reason='bin/test_textattr.exe missing '
                                      '(run: make test_textattr)'))
    else:
        steps.append(Step('test_textattr', [test_textattr], 120,
                          note='colour/size/speed clamping, window bounds, '
                               'configured count'))

    # 4f. save thumbnail scaling (1920x1080 BGRA -> 200x113)
    test_thumb = tools['test_thumb']
    if not os.path.isfile(test_thumb):
        steps.append(Step('test_thumb', None, 0,
                          skip_reason='bin/test_thumb.exe missing '
                                      '(run: make test_thumb)'))
    else:
        steps.append(Step('test_thumb', [test_thumb], 120,
                          note='box filter colour/alpha, 2x2 average, real '
                               '1920x1080 -> 200x113'))

    # 4g. UTF-8 -> CP932 / GBK encoding (needed to write save strings)
    test_encode = tools['test_encode']
    if not os.path.isfile(test_encode):
        steps.append(Step('test_encode', None, 0,
                          skip_reason='bin/test_encode.exe missing '
                                      '(run: make test_encode)'))
    else:
        steps.append(Step('test_encode', [test_encode], 120,
                          note='UTF-8 -> CP932/GBK bytes + decode round-trip, '
                               'unmappable -> ?'))

    # 4h. mouse wheel accumulate/take semantics (InputGetWheel, command 36)
    test_wheel = tools['test_wheel']
    if not os.path.isfile(test_wheel):
        steps.append(Step('test_wheel', None, 0,
                          skip_reason='bin/test_wheel.exe missing '
                                      '(run: make test_wheel)'))
    else:
        steps.append(Step('test_wheel', [test_wheel], 120,
                          note='wheel accumulate / take-clears / negative / '
                               'NULL safety'))

    # 4i. animation tween core (Motion* timeline math, not wired to gfx yet)
    test_motion = tools['test_motion']
    if not os.path.isfile(test_motion):
        steps.append(Step('test_motion', None, 0,
                          skip_reason='bin/test_motion.exe missing '
                                      '(run: make test_motion)'))
    else:
        steps.append(Step('test_motion', [test_motion], 120,
                          note='lerp timeline, zero/negative duration, reap, '
                               'multi-tween, NULL safety'))

    # 4i2. Control* / Cursor* state (ControlMask 8 / ControlPulse 9 /
    #      CursorChange 10 / CursorMove 11): type-driven validation only.
    test_control = tools.get('test_control')
    if not test_control or not os.path.isfile(test_control):
        steps.append(Step('test_control', None, 0,
                          skip_reason='bin/test_control.exe missing '
                                      '(run: make test_control)'))
    else:
        steps.append(Step('test_control', [test_control], 120,
                          note='mask/pulse/cursor state: type-vs-value rules, '
                               '0..3 clamp, NULL args, per-command counters'))

    # 4i3. GraphRGB(23) / SysProjFolder(109) state (recorded, not rendered)
    test_graphcolor = tools.get('test_graphcolor')
    if not test_graphcolor or not os.path.isfile(test_graphcolor):
        steps.append(Step('test_graphcolor', None, 0,
                          skip_reason='bin/test_graphcolor.exe missing '
                                      '(run: make test_graphcolor)'))
    else:
        steps.append(Step('test_graphcolor', [test_graphcolor], 120,
                          note='graph slot RGB: 0..0xFFF slot, 0..200 '
                               'components with 100 fallback; project folder '
                               'string'))

    # 4i4. TextBuff(110) text-window buffer sizes (recorded, not applied)
    test_textbuff = tools.get('test_textbuff')
    if not test_textbuff or not os.path.isfile(test_textbuff):
        steps.append(Step('test_textbuff', None, 0,
                          skip_reason='bin/test_textbuff.exe missing '
                                      '(run: make test_textbuff)'))
    else:
        steps.append(Step('test_textbuff', [test_textbuff], 120,
                          note='TextBuff 110: window 0..31, 8/8 defaults, '
                               '0..0x400 range, whole command dropped >0x400'))

    # 4i0. syscall 覆盖率审计（静态扫四个 switch，缺口数不许变大）
    cov = os.path.join(FVP, 'scripts', 'check-commands.py')
    if not os.path.isfile(cov):
        steps.append(Step('check-commands.py', None, 0,
                          skip_reason='scripts/check-commands.py missing'))
    else:
        steps.append(Step('check-commands.py', [PY, cov], 60,
                          note='148 command ids: handler / generic *Test / '
                               'documented no-op; missing count ratcheted'))

    # 4i5. GaijiLoad(21) / V3DMotion(142): parameter checks + recording
    test_gaiji = tools.get('test_gaiji')
    if not test_gaiji or not os.path.isfile(test_gaiji):
        steps.append(Step('test_gaiji', None, 0,
                          skip_reason='bin/test_gaiji.exe missing '
                                      '(run: make test_gaiji)'))
    else:
        steps.append(Step('test_gaiji', [test_gaiji], 120,
                          note='GaijiLoad 21: strings + size 12..64; '
                               'V3DMotion 142: time 1..300000, mode 1..5'))

    # 4i6. GaijiLoad(21) rendering: registry + "character replaced by an image"
    test_gaiji_render = tools.get('test_gaiji_render')
    if not test_gaiji_render or not os.path.isfile(test_gaiji_render):
        steps.append(Step('test_gaiji_render', None, 0,
                          skip_reason='bin/test_gaiji_render.exe missing '
                                      '(run: make test_gaiji_render)'))
    else:
        steps.append(Step('test_gaiji_render', [test_gaiji_render], 120,
                          note='gaiji registry: codepoint -> graph slot image, '
                               'scaled draw at the text position, re-register '
                               'replaces, empty slot draws nothing'))

    # 4i7. Rand(92) / TextRepaint(124)
    test_vmmisc = tools.get('test_vmmisc')
    if not test_vmmisc or not os.path.isfile(test_vmmisc):
        steps.append(Step('test_vmmisc', None, 0,
                          skip_reason='bin/test_vmmisc.exe missing '
                                      '(run: make test_vmmisc)'))
    else:
        steps.append(Step('test_vmmisc', [test_vmmisc], 120,
                          note='Rand 92: 0..0x7FFF + deterministic per run; '
                               'TextRepaint 124: counter only'))

    # 4i8. null / type-0 值模型（pushfalse → FVP_NULL）
    test_nullvalue = tools.get('test_nullvalue')
    if not test_nullvalue or not os.path.isfile(test_nullvalue):
        steps.append(Step('test_nullvalue', None, 0,
                          skip_reason='bin/test_nullvalue.exe missing '
                                      '(run: make test_nullvalue)'))
    else:
        steps.append(Step('test_nullvalue', [test_nullvalue], 120,
                          note='FVP_NULL = 原版 Variant type 0: pushfalse '
                               'pushes null, null == int 0 in eq/cmp/str, '
                               'LipAnim/LipSync/ControlMask/CursorShow null '
                               'branches reachable, end-to-end 0x09 decode'))

    # 4i9. 标题菜单布局/命中（几何来自原版实测）
    test_titlemenu = tools.get('test_titlemenu')
    if not test_titlemenu or not os.path.isfile(test_titlemenu):
        steps.append(Step('test_titlemenu', None, 0,
                          skip_reason='bin/test_titlemenu.exe missing '
                                      '(run: make test_titlemenu)'))
    else:
        steps.append(Step('test_titlemenu', [test_titlemenu], 120,
                          note='engine-side title menu: 5 items right-aligned '
                               'to x=1769, pitch 100, ink 42px, hover/click '
                               'hit bands'))

    # 4i10. 标题菜单的像素级渲染验证（布局 + gfx_draw_text_line 量墨迹）
    test_titlemenu_draw = tools.get('test_titlemenu_draw')
    if not test_titlemenu_draw or not os.path.isfile(test_titlemenu_draw):
        steps.append(Step('test_titlemenu_draw', None, 0,
                          skip_reason='bin/test_titlemenu_draw.exe missing '
                                      '(run: make test_titlemenu_draw)'))
    else:
        steps.append(Step('test_titlemenu_draw', [test_titlemenu_draw], 120,
                          note='title menu drawn on a 1920x1080 canvas: ink '
                               'right edge ~1769, pitch 100, ink height 40..56, '
                               'hit test matches layout'))

    # 4i11. 标题进场动画的底层：缩放直贴 + 脚本补间收敛到静态绘制
    test_gfx_xform = tools.get('test_gfx_xform')
    if not test_gfx_xform or not os.path.isfile(test_gfx_xform):
        steps.append(Step('test_gfx_xform', None, 0,
                          skip_reason='bin/test_gfx_xform.exe missing '
                                      '(run: make test_gfx_xform)'))
    else:
        steps.append(Step('test_gfx_xform', [test_gfx_xform], 120,
                          note='title entrance animation primitives: identity '
                               'xform == static draw byte for byte, script '
                               'tweens (alpha 0..255 / scale 10000..1000, 1400 '
                               'ms, mode 3) converge to the static frame'))

    # 4i12. 原版 GDI 绘制协议（cp932 + GGO_BITMAP 1bpp）——Windows 专属
    test_gdi_ggo = tools.get('test_gdi_ggo')
    if not test_gdi_ggo or not os.path.isfile(test_gdi_ggo):
        steps.append(Step('test_gdi_ggo', None, 0,
                          skip_reason='bin/test_gdi_ggo.exe missing '
                                      '(run: make test_gdi_ggo)'))
    else:
        steps.append(Step('test_gdi_ggo', [test_gdi_ggo], 120,
                          note='original GDI protocol: binary 1bpp mask, '
                               'gmCellIncX advances, cp932-only (ref/exe-gdi-params.json)'))

    # 4i11. Continue 的存档列表（时间戳/章节名/缩略图，只读）
    test_savelist = tools.get('test_savelist')
    if not test_savelist or not os.path.isfile(test_savelist):
        steps.append(Step('test_savelist', None, 0,
                          skip_reason='bin/test_savelist.exe missing '
                                      '(run: make test_savelist)'))
    else:
        steps.append(Step('test_savelist', [test_savelist], 120,
                          note='continue save list: slot file -> timestamp/'
                               'chapter/line decoded, row text, hit bands, cap'))

    # 4i12. 开场序列时间轴（logo 淡入 -> 停留 -> 淡出 -> 白场）
    test_openseq = tools.get('test_openseq')
    if not test_openseq or not os.path.isfile(test_openseq):
        steps.append(Step('test_openseq', None, 0,
                          skip_reason='bin/test_openseq.exe missing '
                                      '(run: make test_openseq)'))
    else:
        steps.append(Step('test_openseq', [test_openseq], 60,
                          note='opening sequence timeline: phase boundaries, '
                               'alpha monotonic/bounded, clamping'))

    # 4j. motion wired into gfx (disabled by default -> old frames unaffected)
    test_titlebg = tools['test_titlebg']
    if not os.path.isfile(test_titlebg):
        steps.append(Step('test_titlebg', None, 0,
                          skip_reason='bin/test_titlebg.exe missing '
                                      '(run: make test_titlebg)'))
    else:
        steps.append(Step('test_titlebg', [test_titlebg], 120,
                          note='title background zoom timeline (hoshimemo '
                               '2000->1000 per-mille over 40000 ms, ease 3)'))

    test_titleenter = tools['test_titleenter']
    if not os.path.isfile(test_titleenter):
        steps.append(Step('test_titleenter', None, 0,
                          skip_reason='bin/test_titleenter.exe missing '
                                      '(run: make test_titleenter)'))
    else:
        steps.append(Step('test_titleenter', [test_titleenter], 120,
                          note='title entrance per-element timeline (logo '
                               '2000/2000, menu 2400+40i, copyright 2560/820, '
                               'total 4000 ms)'))

    test_gfx_motion = tools['test_gfx_motion']
    if not os.path.isfile(test_gfx_motion):
        steps.append(Step('test_gfx_motion', None, 0,
                          skip_reason='bin/test_gfx_motion.exe missing '
                                      '(run: make test_gfx_motion)'))
    else:
        steps.append(Step('test_gfx_motion', [test_gfx_motion], 120,
                          note='disabled = no movement; enabled = lerp to '
                               'midpoint/target for xy/alpha/scale'))

    # 4k. texture cache LRU cap (long runs must not grow without bound)
    test_tex_cache = tools.get('test_tex_cache')
    if not test_tex_cache or not os.path.isfile(test_tex_cache):
        steps.append(Step('test_tex_cache', None, 0,
                          skip_reason='bin/test_tex_cache.exe missing '
                                      '(run: make test_tex_cache)'))
    else:
        steps.append(Step('test_tex_cache', [test_tex_cache], 120,
                          note='cap evicts LRU refs==0 entries only; '
                               'referenced images survive'))

    # 4l. engine timers (TimerSet/TimerGet/TimerSuspend)
    test_timer = tools.get('test_timer')
    if not test_timer or not os.path.isfile(test_timer):
        steps.append(Step('test_timer', None, 0,
                          skip_reason='bin/test_timer.exe missing '
                                      '(run: make test_timer)'))
    else:
        steps.append(Step('test_timer', [test_timer], 120,
                          note='16 timers: set/elapsed/percent/clamp/overflow'))

    # 4m. typewriter (逐字显示，默认关闭 -> 像素基线不变)
    test_typewriter = tools.get('test_typewriter')
    if not test_typewriter or not os.path.isfile(test_typewriter):
        steps.append(Step('test_typewriter', None, 0,
                          skip_reason='bin/test_typewriter.exe missing '
                                      '(run: make test_typewriter)'))
    else:
        steps.append(Step('test_typewriter', [test_typewriter], 120,
                          note='off = fully shown; on = per-char by TextSpeed; '
                               'UTF-8 bounds, speed 0, multi-window'))

    # 4n. 台词历史（HistorySet 26 / HistoryGet 25 的环形缓冲）
    test_history = tools.get('test_history')
    if not test_history or not os.path.isfile(test_history):
        steps.append(Step('test_history', None, 0,
                          skip_reason='bin/test_history.exe missing '
                                      '(run: make test_history)'))
    else:
        steps.append(Step('test_history', [test_history], 120,
                          note='100-slot ring: count cap, pos wrap, slot = pos-idx, '
                               'kind/type filters, NULL safety'))

    # 4n3. 槽尾动态字符串表（位置 = 0x163A0 + 全局变量字节数：src/savetail.c）
    test_savetail = tools.get('test_savetail')
    if not test_savetail or not os.path.isfile(test_savetail):
        steps.append(Step('test_savetail', None, 0,
                          skip_reason='bin/test_savetail.exe missing '
                                      '(run: make test_savetail)'))
    else:
        steps.append(Step('test_savetail', [test_savetail], 120,
                          note='slot-tail dynamic string table: header (thread '
                               'index 0x20 + non-empty count) located at '
                               '0x163A0 + globals_bytes = 0x19408 in the 6 real '
                               'saves, not a fixed offset; 0/1549/100 globals '
                               'synthetic cases, empty table, malformed inputs '
                               'and two-candidate ambiguity rejected'))

    # 4n2. 槽尾台词历史环（读档 step-2 第一段：src/savehist.c）
    test_savehist = tools.get('test_savehist')
    if not test_savehist or not os.path.isfile(test_savehist):
        steps.append(Step('test_savehist', None, 0,
                          skip_reason='bin/test_savehist.exe missing '
                                      '(run: make test_savehist)'))
    else:
        steps.append(Step('test_savehist', [test_savehist], 120,
                          note='slot-tail dialogue history ring: synthetic '
                               'locate/parse, malformed inputs rejected, '
                               'count==100 treated as ambiguous, apply into '
                               'FvpHistory; real s905 slot -> count=11/pos=10'))

    # 4o. 命中测试（PrimHit 76 → gfx_prim_hit）
    test_prim_hit = tools.get('test_prim_hit')
    if not test_prim_hit or not os.path.isfile(test_prim_hit):
        steps.append(Step('test_prim_hit', None, 0,
                          skip_reason='bin/test_prim_hit.exe missing '
                                      '(run: make test_prim_hit)'))
    else:
        steps.append(Step('test_prim_hit', [test_prim_hit], 120,
                          note='parent-chain offsets, draw gating, min(prim,image) WH, '
                               'UV clip, open right/bottom edges, group recursion'))

    # 4p. 跨图集字形兜底（缺字时缩放取别的图集，不再画豆腐块）
    test_glyph_fallback = tools.get('test_glyph_fallback')
    if not test_glyph_fallback or not os.path.isfile(test_glyph_fallback):
        steps.append(Step('test_glyph_fallback', None, 0,
                          skip_reason='bin/test_glyph_fallback.exe missing '
                                      '(run: make test_glyph_fallback)'))
    else:
        steps.append(Step('test_glyph_fallback', [test_glyph_fallback], 120,
                          note='hit / scaled fallback / tofu counters, '
                               'per-render reset'))

    # 5. VM edges
    vm_edge = os.path.join(HERE, 'test_vm_edge.py')
    guard = hb.find_cc()
    steps.append(Step('test_vm_edge.py', [PY, vm_edge], 900,
                      note='43 boundary cases, fvm.exe + fvm_guard.exe'))

    # 6. HCB fuzz
    fuzz_hcb = os.path.join(HERE, 'fuzz_hcb.py')
    hcb_argv = [PY, fuzz_hcb]
    if args.fast:
        hcb_argv.append('--fast')
    steps.append(Step('fuzz_hcb.py%s' % (' --fast' if args.fast else ''),
                      hcb_argv, 3600,
                      skip_reason=None if guard else
                      'gcc unavailable: in-process harness cannot be built',
                      note='>= 20,000 mutations + fvm.exe E2E'))

    # 6.4 与原版实拍帧的回归（缺语料时自己打印 SKIP 并返回 0）
    vvo = os.path.join(HERE, 'verify_vs_original.py')
    steps.append(Step('verify_vs_original.py', [PY, vvo], 900,
                      skip_reason=None,
                      note='title screen vs the original capture: mean abs diff baseline'))

    # 6.45 原版 exe 的 GDI 绘制协议（静态提取；缺 exe/静态分析时自己 SKIP）
    vgp = os.path.join(HERE, 'verify_exe_gdi_params.py')
    steps.append(Step('verify_exe_gdi_params.py', [PY, vgp], 300,
                      skip_reason=None,
                      note='static extraction of the original GDI text protocol '
                           '(charset/GGO format/MAT2) must stay byte-identical'))

    # 6.47 脚本侧实参审计（命令表 argc 与真实调用点对账；缺 HCB 时自己 SKIP）
    vsa = os.path.join(HERE, 'verify_script_argc.py')
    steps.append(Step('verify_script_argc.py', [PY, '-X', 'utf8', vsa], 900,
                      skip_reason=None,
                      note='command-table argc cross-checked against real call '
                           'sites in both works (TitleMenu = 1 bool; 3 known '
                           'expression-temp anomalies)'))

    # 6.48 标题画面场景的脚本侧事实（素材/槽位/菜单项图元号；缺 HCB 时自己 SKIP）
    vts = os.path.join(HERE, 'verify_title_scene.py')
    steps.append(Step('verify_title_scene.py', [PY, '-X', 'utf8', vts], 900,
                      skip_reason=None,
                      note='title screen is script-drawn: title_* GraphLoads + '
                           'menu items are prims (iroseka 1501..1529 / hoshimemo '
                           '266..274) hit-tested by PrimHit'))

    # 6.49 标题按钮矩形（来自 graph.bin 的素材头；缺游戏目录时自己 SKIP）
    vtr = os.path.join(HERE, 'verify_title_rects.py')
    steps.append(Step('verify_title_rects.py', [PY, '-X', 'utf8', vtr], 1800,
                      skip_reason=None,
                      note='title button rects come from the HZC1 asset headers: '
                           'iroseka right edge 1785 / pitch 100, hoshimemo centre '
                           '960 / pitch 90'))

    # 6.495 标题画面误差分布（总平均差拆到 logo/menu/copyright/rest；
    #       需要 Pillow+numpy，缺了会自己 SKIP）
    vtd = os.path.join(HERE, 'verify_title_diff.py')
    steps.append(Step('verify_title_diff.py', [PY, '-X', 'utf8', vtd], 900,
                      skip_reason=None,
                      note='title diff broken down by region (asset-header rects); '
                           'copyright region is the worst, ranked baseline frozen'))

    # 6.496 Motion*/PrimSet* 的实参约束（从 exe handler 静态分析提取；缺 work/ 时 SKIP）
    vmp = os.path.join(HERE, 'verify_exe_motion_params.py')
    steps.append(Step('verify_exe_motion_params.py', [PY, '-X', 'utf8', vmp], 600,
                      skip_reason=None,
                      note='argument constraints from the exe handlers: '
                           'MotionMoveS2 = 8 args (scale 100..10000, dur 1..300000, '
                           'mode 1..5), Stop only cancels; PrimSetRS angle unit 3600'))

    # 6.4964 图元结构/绘制派发/父链累积（从 exe 0x438AB0 静态分析提取；缺 work/ 时 SKIP）
    vph = os.path.join(HERE, 'verify_exe_prim_hierarchy.py')
    steps.append(Step('verify_exe_prim_hierarchy.py', [PY, '-X', 'utf8', vph], 600,
                      skip_reason=None,
                      note='prim struct (stride 0x34, base engine+0x61DD64, 4096 entries),'
                           ' dispatch 0x438AB0 (7 types, parent chain sums x/y) and the'
                           ' type-5 text path (layer +0x32, window table +0x98, surface +4)'))

    # 6.49645 引擎自建对象表（14 张表的 base/size/count/ctor；缺 work/ 时 SKIP）
    veo = os.path.join(HERE, 'verify_exe_engine_objects.py')
    steps.append(Step('verify_exe_engine_objects.py', [PY, '-X', 'utf8', veo], 600,
                      skip_reason=None,
                      note='engine object tables read from the exe Construct() sites: '
                           '14 tables (sprite 4096x0x51C, prim_alt 64x0x34, prim 4096x0x34 '
                           'contiguous), 7 container ctors cross-checked against the '
                           'registration list'))

    # 6.49646 脚本侧窗口几何（设计 1024x640 → 画布 1920x1080；缺游戏数据时 SKIP）
    vsw = os.path.join(HERE, 'verify_script_window_geometry.py')
    steps.append(Step('verify_script_window_geometry.py', [PY, '-X', 'utf8', vsw], 600,
                      skip_reason=None,
                      note='text-window geometry read from the HCB: design space '
                           '1024x640 scaled by 1.875 (1920/1024) and 1.6875 (1080/640) '
                           'inside the script helpers, TextBuff clamped to 1024, and the '
                           'four window bindings 552/8, 551/0, 558/10, 553/9'))

    # 6.49647 帧槽模型（默认保持冻结文本基线；FVP_FRAME_SLOTS=1 走修正模型）
    vfs = os.path.join(HERE, 'verify_frame_slots.py')
    steps.append(Step('verify_frame_slots.py', [PY, '-X', 'utf8', vfs], 1800,
                      skip_reason=None,
                      note='frame slot model: default keeps ref/ref-900k.txt byte-identical; '
                           'PrimSetText(551) must NEVER be (1,1) (that was the pushtrue '
                           'Variant-type-1 sentinel bug) and 552/553 coordinates are the '
                           'regression guard; FVP_FRAME_SLOTS=1 puts the '
                           'body window at the script value (431,880) and changes the text '
                           'output (6760 B) — re-freezing that baseline needs a decision'))

    # 6.49648 文本面板落点/尺寸（从 exe 0x42AE40 抄出来；缺 work/ 时 SKIP）
    vtd = os.path.join(HERE, 'verify_exe_text_draw.py')
    steps.append(Step('verify_exe_text_draw.py', [PY, '-X', 'utf8', vtd], 600,
                      skip_reason=None,
                      note='text panel placement from the exe: size = prim w/h if the WH flag '
                           'is set else the surface size (PrimSetText clears WH, so text uses '
                           'the panel), position = prim + OP (or surface +0x518/+0x51a) + the '
                           'parent-chain sum -> the panel is drawn 1:1 at the prim (x,y)'))

    # 6.49649 文本必须来自 HCB 字符串（冻结基线 0 编造；FVP_FRAME_SLOTS=1 多读字符）
    vth = os.path.join(HERE, 'verify_text_from_hcb.py')
    steps.append(Step('verify_text_from_hcb.py', [PY, '-X', 'utf8', vth], 900,
                      skip_reason=None,
                      note='every text line of the frozen baseline must exist verbatim in the '
                           'HCB (0 fabricated); the FVP_FRAME_SLOTS=1 model over-reads one '
                           'character per line (70 lines not in the HCB), so that baseline '
                           'must not be re-frozen as-is'))

    # 6.49650 脚本侧文本窗口几何 + pushtrue 的 Variant type 1 语义
    vwc = os.path.join(HERE, 'verify_script_window_calls.py')
    steps.append(Step('verify_script_window_calls.py', [PY, '-X', 'utf8', vwc], 600,
                      skip_reason=None,
                      note='script-side window geometry: 6 call sites of the window-setup '
                           'function, only 0x0367BD passes explicit (246,258,800,320) -> canvas '
                           '(461.25,435.375); the other 5 use defaults (230,522,736,120) -> '
                           '(431.25,880.875) panel 1380x1024; plus pushtrue = Variant type 1 '
                           '(so `PrimSetXY(prim,true,y)` really keeps x)'))

    # 6.496495 TextBuff 尺寸上限/越界行为（从 exe 0x430EA0 抄出来；缺 work/ 时 SKIP）
    vtb = os.path.join(HERE, 'verify_exe_textbuff.py')
    steps.append(Step('verify_exe_textbuff.py', [PY, '-X', 'utf8', vtb], 600,
                      skip_reason=None,
                      note='TextBuff limit: w/h each capped at 0x400 and the over-limit '
                           'branch THROWS (code 0x80 at 0x430FF8 -> 0x447C05 -> 0x4258F0 -> '
                           '0x44837C) instead of silently returning'))
    # 6.496496 文本类 handler 实参规则（TextBuff/TextColor/TextOutSize/TextSize；缺 work/ 时 SKIP）
    vth2 = os.path.join(HERE, 'verify_exe_text_handlers.py')
    steps.append(Step('verify_exe_text_handlers.py', [PY, '-X', 'utf8', vth2], 600,
                      skip_reason=None,
                      note='text handler argument rules from the exe: layer must be an int in '
                           '0..0x1F; every other component is written only when it is an int '
                           'inside its range (TextColor 0..0xFF, TextOutSize 0..0xC/0..0x8, '
                           'TextSize 0xC..0x40/0x8..0x20), otherwise it keeps the old value'))

    # 6.49658 ColorSet(7) = 256xBGRA 调色板（engine+0x6A44C0）
    vcs = os.path.join(HERE, 'verify_exe_colorset.py')
    steps.append(Step('verify_exe_colorset.py', [PY, '-X', 'utf8', vcs], 600,
                      skip_reason=None,
                      note='ColorSet(7) writes the 256xBGRA palette at engine+0x6A44C0 '
                           '(index 0..0xFF, per-component int); the same buffer is the '
                           '0x400-byte memcpy in the load restore order'))

    # 6.49657 GraphLoad(22)：type 0 = 释放槽（0x424150）
    vgl = os.path.join(HERE, 'verify_exe_graph_load.py')
    steps.append(Step('verify_exe_graph_load.py', [PY, '-X', 'utf8', vgl], 600,
                      skip_reason=None,
                      note='GraphLoad(22) handler 0x43A340: slot 0..0xFFF; arg1 type 0 '
                           'releases the slot via 0x424150, type 4/5 resolve to a string '
                           'and load through 0x428BD0'))

    # 6.49656 七条 *Test 命令的容器表 + C 侧接线（44/50/56）
    vmt = os.path.join(HERE, 'verify_exe_motion_tests.py')
    steps.append(Step('verify_exe_motion_tests.py', [PY, '-X', 'utf8', vmt], 600,
                      skip_reason=None,
                      note='*Test family: 44/47/50/53/56/59/145 each query a different '
                           'container (0x651E04/0x65380C/0x65BA54/0x679A5C/0x67CE64/'
                           '0x680A6C/0x683F04) and write 1=still-running into '
                           'thread+0x814; 44/50/56 are wired to our three tween kinds'))

    # 6.49655 两部作品实际用到的命令集合 vs 引擎实现（兼容性矩阵）
    vcu = os.path.join(HERE, 'verify_command_usage.py')
    steps.append(Step('verify_command_usage.py', [PY, '-X', 'utf8', vcu], 1200,
                      skip_reason=None,
                      note='both games use 131 distinct command ids; every one of them '
                           'is implemented in src/syscall.c (PartsMotionTest(69) was the '
                           'last gap and is implemented now); AudioLoad(0) is the only '
                           'id still missing from the dispatch baseline'))

    # 6.49654 雪系统记录的字段访问图（snow = prim type 7）
    vss = os.path.join(HERE, 'verify_exe_snow_system.py')
    steps.append(Step('verify_exe_snow_system.py', [PY, '-X', 'utf8', vss], 600,
                      skip_reason=None,
                      note='snow system record (engine+0x684378, stride 0x5048): every '
                           'field access inside the draw fn 0x42AA10, plus the four '
                           'device vtable methods it calls (0xE4/0x104/0x10C/0x164)'))

    # 6.49653 图元类型分派（type 1..7，含 snow 的雪系统结构）
    vptd = os.path.join(HERE, 'verify_exe_prim_type_dispatch.py')
    steps.append(Step('verify_exe_prim_type_dispatch.py', [PY, '-X', 'utf8', vptd], 600,
                      skip_reason=None,
                      note='prim type dispatch: type = (int8)prim+4, index = type-1, '
                           'jump table at 0x438C9C for types 1..7 (3 and 6 are empty); '
                           'type 5 -> 0x42AE40 (text panel), type 7 -> 0x42AA10 (snow, '
                           'selects a 0x5048-byte system record at engine+0x684378 by '
                           'prim+0x2E)'))

    # 6.49652 命令 id → handler（原版注册构造函数 0x43E000..0x440E00；缺 exe 时 SKIP）
    vcd = os.path.join(HERE, 'verify_exe_cmd_dispatch.py')
    steps.append(Step('verify_exe_cmd_dispatch.py', [PY, '-X', 'utf8', vcd], 600,
                      skip_reason=None,
                      note='command id -> handler: the registrar at 0x4387D0 is called once '
                           'per command from 0x43E000..0x440E00 with (handler, argc, name); '
                           'names are alphabetical, so the ids come from the HCB command '
                           'table. 133/148 joined and cross-checked against the text and '
                           'prim setter baselines'))

    # 6.49651 图元字段的位宽（写 u16 + 读 movswl => int16）
    vpfw = os.path.join(HERE, 'verify_exe_prim_fields_width.py')
    steps.append(Step('verify_exe_prim_fields_width.py', [PY, '-X', 'utf8', vpfw], 600,
                      skip_reason=None,
                      note='prim field width: every field is written as u16 by its '
                           'setter handler and read back with movswl in the draw path '
                           '=> int16 (-32768..32767); this is why syscall.c truncates'))

    # 6.49650 图元 setter handler 的实参规则（angle mod 3600 / scale 100..10000）
    vpsa = os.path.join(HERE, 'verify_exe_prim_setters_args.py')
    steps.append(Step('verify_exe_prim_setters_args.py', [PY, '-X', 'utf8', vpsa], 600,
                      skip_reason=None,
                      note='prim setter handlers: 0x43BA90 (op1/op2, flags|=2 then |=0x40), '
                           '0x43BAF0 (angle mod 3600, scale clamped 100..10000), '
                           '0x43BC40/0x43BD00 (create type-7/type-4 prim, then x/y)'))

    # 6.496499 图元字段写入指令全量扫描（定位 prim setter handler 区；缺 work/ 时 SKIP）
    vps = os.path.join(HERE, 'verify_exe_prim_setters.py')
    steps.append(Step('verify_exe_prim_setters.py', [PY, '-X', 'utf8', vps], 600,
                      skip_reason=None,
                      note='every instruction that stores into the 0x34-stride prim '
                           'struct: 57 stores, all inside 0x43BACA..0x43BFDA (the '
                           'prim setter handler block); command id -> handler mapping '
                           'still needs the dispatch table'))

    # 6.496498 全局变量 Variant 的类型语义（exe handler + 存档数据双证据；缺 work/ 时 SKIP）
    vvt = os.path.join(HERE, 'verify_exe_variant_types.py')
    steps.append(Step('verify_exe_variant_types.py', [PY, '-X', 'utf8', vvt], 600,
                      skip_reason=None,
                      note='variant type codes: 0 = null/unset, 2 = int, '
                           '4 = absolute HCB offset string, 5 = dynamic-string slot '
                           'number, 6 = index into the 0x6A4DF0 table (handler '
                           '0x445CB0); each with evidence'))

    # 6.496497 文本窗口的存档格式（从 exe 0x433850 抄出来；缺 work/ 时 SKIP）
    vws = os.path.join(HERE, 'verify_exe_text_window_save.py')
    steps.append(Step('verify_exe_text_window_save.py', [PY, '-X', 'utf8', vws], 600,
                      skip_reason=None,
                      note='text window save format read out of 0x433850: presence byte -> '
                           'surface record -> text string -> u32 count + u16 cells -> '
                           '0x28-byte style block -> two vectors -> panel w/h (0x5DC/0x5DE) '
                           '-> pen x/y (0x5E8/0x5EA)'))

    # 6.4965 D3D9 渲染状态常量（从 exe 的 SetRenderState 调用点抄出来；缺 work/ 时 SKIP）
    vds = os.path.join(HERE, 'verify_exe_d3d_states.py')
    steps.append(Step('verify_exe_d3d_states.py', [PY, '-X', 'utf8', vds], 600,
                      skip_reason=None,
                      note='D3D9 render states from the exe: SRCBLEND=SRCALPHA, '
                           'DESTBLEND=INVSRCALPHA (or ONE when the prim+8 byte is 1), '
                           'ALPHABLENDENABLE=TRUE, BLENDOP never set (=ADD)'))

    # 6.4966 D3D9 设备创建参数 / D3DPRESENT_PARAMETERS（缺 work/ 时 SKIP）
    vdp = os.path.join(HERE, 'verify_exe_d3d_present.py')
    steps.append(Step('verify_exe_d3d_present.py', [PY, '-X', 'utf8', vdp], 600,
                      skip_reason=None,
                      note='D3D9 device creation read out of the exe: windowed '
                           'BackBufferFormat=D3DFMT_UNKNOWN + COPY + Windowed, '
                           'fullscreen A8R8G8B8, HAL + software vertex processing'))

    # 6.4966b 文本样式命令的字段/范围（从 exe handler 提取；缺 work/ 时 SKIP）
    vtp = os.path.join(HERE, 'verify_exe_text_params.py')
    steps.append(Step('verify_exe_text_params.py', [PY, '-X', 'utf8', vtp], 600,
                      skip_reason=None,
                      note='text style commands read out of the exe handlers: '
                           'layer 0..31, TextFormat/TextSpace share +0x5A8/+0x5AA, '
                           'TextSkip +0x5D7, TextShadowDist clamped 0..12, '
                           'TextFont -5..font_count-1'))

    # 6.4966c 文本样式字段的访问地图（读/写/读改写点；缺 work/ 时 SKIP）
    vtf = os.path.join(HERE, 'verify_exe_text_fields.py')
    steps.append(Step('verify_exe_text_fields.py', [PY, '-X', 'utf8', vtf], 600,
                      skip_reason=None,
                      note='text style field access map from the exe: writes only '
                           'from the style handlers + one struct copy routine, '
                           'TextShadowDist byte is written but never read, '
                           '+0x5E8 is an accumulated line width'))

    # 6.4966c2 文本排版笔位链（TextFormat arg4 -> +0x5E8；0x5A8/0x5AA 死存储）
    vtl = os.path.join(HERE, 'verify_exe_text_layout.py')
    steps.append(Step('verify_exe_text_layout.py', [PY, '-X', 'utf8', vtl], 300,
                      skip_reason=None,
                      note='text pen-x chain: TextFormat arg4 -> tw+0x5E8 is pushed, '
                           'wrapped on and restored; +0x5A8/+0x5AA (TextSpace) are '
                           'written but never read'))

    # 6.4966d 文本窗口默认值（来自 exe 构造函数 0x430EA0；缺 work/ 时 SKIP）
    vtd = os.path.join(HERE, 'verify_exe_text_defaults.py')
    steps.append(Step('verify_exe_text_defaults.py', [PY, '-X', 'utf8', vtd], 600,
                      skip_reason=None,
                      note='text window defaults read out of the exe constructor: '
                           'size 16x12, font ids -2/-2, size limit 1024, line '
                           'width accumulator starts from +0x5AC'))

    # 6.4967 标题进场动画的参数必须等于脚本常量（纯 JSON/源码检查，无依赖）
    vta = os.path.join(HERE, 'verify_title_anim.py')
    steps.append(Step('verify_title_anim.py', [PY, '-X', 'utf8', vta], 300,
                      skip_reason=None,
                      note='title entrance animation params == the script '
                           'constants (logo alpha 0->255 / scale 10000->1000, '
                           '1400 ms, mode 3), animation off by default'))

    # 6.4968 标题进场动画的端到端验证（真的启动外壳跑标题；缺游戏数据时 SKIP）
    vtar = os.path.join(HERE, 'verify_title_anim_render.py')
    steps.append(Step('verify_title_anim_render.py', [PY, '-X', 'utf8', vtar], 900,
                      skip_reason=None,
                      note='title entrance animation end to end: mid-animation '
                           'frame differs (mean_abs 14.15), frame after 1400 ms '
                           'is byte identical to the no-animation frame'))

    # 6.49685 标题进场逐元素时间轴 == 脚本常量（纯 JSON/源码检查，无依赖）
    vte = os.path.join(HERE, 'verify_title_enter.py')
    steps.append(Step('verify_title_enter.py', [PY, '-X', 'utf8', vte], 300,
                      skip_reason=None,
                      note='title entrance timeline == script constants for both games '
                           '(iroseka 2000/2400/2560 total 4000; '
                           'hoshimemo 1000/1400/1560 total 3000)'))

    # 6.496862 读档续玩 step-1（--load-restart：读槽 + 场景复位 + 脚本重启）
    vlr = os.path.join(HERE, 'verify_load_restart.py')
    steps.append(Step('verify_load_restart.py', [PY, '-X', 'utf8', vlr], 300,
                      skip_reason=None,
                      note='load restart step 1: reads slot file (thumb '
                           '200x113), resets scene (prims/motion/text), '
                           'restarts the script engine at entry keeping '
                           'globals/flags; script keeps running after restart'))

    # 6.496863 存档/读档引擎侧流程（Load(40)/SaveWrite(96)）
    vlf = os.path.join(HERE, 'verify_exe_load_flow.py')
    steps.append(Step('verify_exe_load_flow.py', [PY, '-X', 'utf8', vlf], 300,
                      skip_reason=None,
                      note='save/load engine flow frozen: path fmt '
                           '"%s/save/s%03d.bin", Load leaves engine+0xFCB/0xFCC, '
                           'frame consumer 0x43F876 -> 0x43EE90 -> thread+0xFD9 '
                           'restart; script sites 9/3/7 for both games'))

    # 6.4968635 存档槽序列化顺序（0x43DE90 的 31 段写入顺序 + 6 份存档校验）
    vss = os.path.join(HERE, 'verify_exe_save_serialize.py')
    steps.append(Step('verify_exe_save_serialize.py', [PY, '-X', 'utf8', vss],
                      300, skip_reason=None,
                      note='slot write order 31 segments + read order 20 '
                           'steps frozen in ref/exe-save-serialize.json; data '
                           'side validated on 6 real saves: tail[0:0x3068] = '
                           '1549 per-slot globals, 32 contexts x 0xB1D = 0x163A0, '
                           'thread index + 128-slot string table at 0x19408, '
                           'walk lands exactly on the history ring (no gap; the ring always writes 100 '
                           'records, then the gaiji table = 4 groups/12 records/0x15a '
                           'and the 4096-item table first item; record offsets match an '
                           'independent scan), last two bytes 01 01'))

    # 6.4968636 台词历史（HistorySet 26 / HistoryGet 25）的 handler + 槽尾历史环
    vhi = os.path.join(HERE, 'verify_exe_history.py')
    steps.append(Step('verify_exe_history.py', [PY, '-X', 'utf8', vhi], 600,
                      skip_reason=None,
                      note='history frozen: HistorySet 0x43A5E0 / HistoryGet '
                           '0x43A490 (Variant type 0 = begin, kind 0..2, '
                           'idx < count), container count starts at 1, ring '
                           'writer 0x437FA0 / reader 0x437EE0; 6 real saves '
                           'parse their slot-tail ring (s481 count=1, '
                           's905 count=11)'))

    # 6.496864 脚本全局开关（global3 体验版布局 / global236/232 跳过动画）
    vg = os.path.join(HERE, 'verify_script_globals.py')
    steps.append(Step('verify_script_globals.py', [PY, '-X', 'utf8', vg], 300,
                      skip_reason=None,
                      note='script globals frozen: g3 = trial-layout switch '
                           '(single writer = 0 in retail scripts -> branch '
                           'unreachable), g236/g232 = skip-animation switch'))

    # 6.496865 RS 分支变换 + 旋转角单位（exe 静态分析 → 基线 → 源码一致）
    vrs = os.path.join(HERE, 'verify_exe_rs_transform.py')
    steps.append(Step('verify_exe_rs_transform.py',
                      [PY, '-X', 'utf8', vrs], 300, skip_reason=None,
                      note='rotation unit = 0.05 deg/unit (7200 per turn, '
                           'exe 0x42B1E2 /3600*pi) and the RS-branch transform '
                           'final = T2 + S*(R*(img-OP)+prim+accum) frozen in '
                           'ref/exe-rs-transform.json'))

    # 6.49687 标题背景缩放的端到端门禁（星空；含一次 42 s 运行验证终态）
    vbgz = os.path.join(HERE, 'verify_title_bg_anim.py')
    steps.append(Step('verify_title_bg_anim.py', [PY, '-X', 'utf8', vbgz], 900,
                      skip_reason=None,
                      note='star sky title background zoom end to end: mid '
                           'frame == zoomed (mean_abs 48.1), 40 s later byte '
                           'identical to static, ~48 fps with the zoom cache'))

    # 6.49688 标题背景的缩放/旋转参数（旋转在脚本里，不在图像包里）
    vbg = os.path.join(HERE, 'verify_title_bg_motion.py')
    steps.append(Step('verify_title_bg_motion.py', [PY, '-X', 'utf8', vbg], 300,
                      skip_reason=None,
                      note='title background zoom/rotation come from the HCB '
                           'Motion commands (iroseka 15/25/55/125 deg -> 0 over '
                           '25 s; hoshimemo zoom 2000->1000 over 40 s, no '
                           'rotation); HZC1 headers carry no rotation'))

    # 6.4969 开场 FAVORITE logo 的时间轴 == 脚本常量（纯 JSON/源码检查，无依赖）
    voa = os.path.join(HERE, 'verify_opening_anim.py')
    steps.append(Step('verify_opening_anim.py', [PY, '-X', 'utf8', voa], 300,
                      skip_reason=None,
                      note='opening timeline == script constants (3800 ms, '
                           'six layers, per-game OP pivot), both FVP titles'))

    # 6.49695 开场端到端：真的启动外壳抓帧（缺游戏数据时 SKIP）
    vor = os.path.join(HERE, 'verify_opening_render.py')
    steps.append(Step('verify_opening_render.py', [PY, '-X', 'utf8', vor], 900,
                      skip_reason=None,
                      note='opening end to end: starts white, the fade reveals '
                           'the title background (script sets the title up '
                           'before the opening), end frame == static title'))

    # 6.497 标题背景合成模型（18 个候选里 measured+over 最好；需 Pillow+numpy）
    vbb = os.path.join(HERE, 'verify_title_bg_blend.py')
    steps.append(Step('verify_title_bg_blend.py', [PY, '-X', 'utf8', vbb], 900,
                      skip_reason=None,
                      note='background composite model validated against the '
                           'original frame: order A->C->D->B + straight-alpha '
                           'over beats z-orderings and premul/add'))

    # 6.50 标题菜单状态变体（整幅不同的状态/动画帧，不是轻微高亮）
    vtv = os.path.join(HERE, 'verify_title_variants.py')
    steps.append(Step('verify_title_variants.py', [PY, '-X', 'utf8', vtv], 1800,
                      skip_reason=None,
                      note='title state variants are whole-image frames '
                           '(iroseka 5/item, hoshimemo 1/item), so hover cannot '
                           'be a simple variant swap'))

    # 6.55 游戏内菜单的三个动作（SAVE/LOAD/OPTION）
    vma = os.path.join(HERE, 'verify_menu_actions.py')
    steps.append(Step('verify_menu_actions.py', [PY, vma], 900,
                      skip_reason=None,
                      note='menu SAVE writes a template slot, LOAD opens the list, OPTION panel renders'))

    # 6.47 游戏内菜单的静态结构（脚本侧：命令表里没有 Menu* 命令）
    vim = os.path.join(HERE, 'verify_ingame_menu.py')
    steps.append(Step('verify_ingame_menu.py', [PY, '-X', 'utf8', vim], 300,
                      skip_reason=None,
                      note='in-game menu is script-side: 8 ring items sys_* (prims 141..164) + '
                           '7 confirm prompts menu_*_mes (prim 94, yes/no 95..98); '
                           '98 UI asset load sites, geometry from HZC1 headers'))

    # 6.48 跨作品 UI 素材对账（两作同一套命名；差异只有 _M / window_title / prt_mask）
    vuc = os.path.join(HERE, 'verify_ui_cross.py')
    steps.append(Step('verify_ui_cross.py', [PY, '-X', 'utf8', vuc], 300,
                      skip_reason=None,
                      note='UI asset names across both games: 98 vs 90 loads, '
                           'identical menu_/fc_, iroseka-only 8 sys_*_M + window_title, '
                           'hoshimemo-only slf_prt_mask'))

    # 6.49 UI 素材几何（直接读 .bin 归档里的 HZC1 头；两作都做，五彩与解包路径互证）
    vur = os.path.join(HERE, 'verify_ui_rects.py')
    steps.append(Step('verify_ui_rects.py', [PY, '-X', 'utf8', vur], 600,
                      skip_reason=None,
                      note='UI geometry straight from the .bin archives (no unpack): '
                           'iroseka 81 rects cross-checked against the extract path, '
                           'hoshimemo 73 rects (8 ring items 125x125, full-height windows)'))

    # 6.5 游戏内菜单（外壳侧占位实现，--menu-at 打开）
    vg = os.path.join(HERE, 'verify_gamemenu.py')
    steps.append(Step('verify_gamemenu.py', [PY, vg], 900,
                      skip_reason=None,
                      note='placeholder shell overlay (5 text items); the original '
                           'menu is drawn by script, see verify_ingame_menu.py'))

    # 6.6 headless 外壳（不依赖 Win32 窗口/D3D）
    vh = os.path.join(HERE, 'verify_headless.py')
    steps.append(Step('verify_headless.py', [PY, vh], 1800,
                      skip_reason=None,
                      note='portable platform seam + offscreen render, steps match fvm'))

    # 7. asset fuzz
    fuzz_assets = os.path.join(HERE, 'fuzz_assets.py')
    fa_argv = [PY, fuzz_assets]
    if args.fast:
        fa_argv.append('--fast')
    steps.append(Step('fuzz_assets.py%s' % (' --fast' if args.fast else ''),
                      fa_argv, 3600,
                      skip_reason=None,
                      note='BIN/HZC1/inflate/WAV/Ogg malformed inputs'))
    if args.keep:
        for s in steps:
            if s.argv and s.name.startswith('fuzz_'):
                s.argv.append('--keep')
    return steps


def save_failed_attempt(step_name, out):
    """把"第一次失败"的完整输出落盘，返回路径（供 FLAKY 事后诊断）。

    之前只保留了重试成功那次的输出，FLAKY 就只剩一句"failed once"，无法定位是哪条
    断言、什么数值。现在第一次失败的原样输出会写到 out/run_all-<step>-attempt1.txt。
    """
    safe = ''.join(c if (c.isalnum() or c in '-_.') else '_'
                   for c in step_name)[:80]
    path = os.path.join(OUT, 'run_all-%s-attempt1.txt' % safe)
    try:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(out or '')
    except OSError:
        return None
    return path


def failed_lines(out, limit=6):
    """从输出里挑出 FAIL/RESULT: FAIL 行，拼成一句话。"""
    picked = []
    for line in (out or '').splitlines():
        t = line.strip()
        if t.startswith('FAIL') or 'RESULT: FAIL' in t:
            picked.append(t[:160])
            if len(picked) >= limit:
                break
    return ' | '.join(picked)


def run_step(step, retry=True):
    """Run one step.  A failing step is retried once (many steps are timing
    sensitive); the result records how many attempts it took and whether the
    first attempt failed, so flakiness is visible instead of silent."""
    if step.argv is None:
        return {'name': step.name, 'status': 'SKIP',
                'reason': step.skip_reason or '', 'elapsed': 0.0, 'note': ''}
    first = _run_step_once(step)
    if retry and first['status'] == 'FAIL':
        again = _run_step_once(step)
        again['attempts'] = 2
        again.pop('out', None)
        again['first_attempt_failed'] = True
        if again['status'] == 'PASS':
            path = save_failed_attempt(step.name, first.get('out', ''))
            why = failed_lines(first.get('out', ''))
            again['note'] = ('FLAKY: failed once, passed on retry. ' +
                             (('first attempt failed at: ' + why + '. ')
                              if why else '') +
                             ('first-attempt log: %s. ' % path if path else '') +
                             (again.get('note') or ''))
        return again
    first.pop('out', None)
    first['attempts'] = 1
    first['first_attempt_failed'] = first['status'] == 'FAIL'
    return first


def _run_step_once(step):
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(step.argv, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=step.timeout)
        out = proc.stdout.decode('utf-8', 'replace')
        rc = proc.returncode
        elapsed = time.perf_counter() - t0
        status = 'PASS' if rc == 0 else 'FAIL'
        # A script that exits 0 but printed RESULT: FAIL is still a failure.
        if status == 'PASS' and 'RESULT: FAIL' in out:
            status = 'FAIL'
        note = note_from(out, ('mutant executions:', 'cases=', 'peak=',
                               'byte-identical', 'OVERALL:', 'RESULT:')) or \
            step.note
        return {'name': step.name, 'status': status, 'elapsed': elapsed,
                'note': note, 'tail': '' if status == 'PASS' else tail(out),
                'exit': rc, 'out': out}
    except subprocess.TimeoutExpired:
        return {'name': step.name, 'status': 'TIMEOUT', 'elapsed': step.timeout,
                'note': 'exceeded %d s' % step.timeout, 'tail': '', 'exit': None}
    except OSError as exc:
        return {'name': step.name, 'status': 'ERROR', 'elapsed': 0.0,
                'note': str(exc), 'tail': '', 'exit': None}


def run_selfcheck():
    """Run the allocator self-test binaries directly (fast, no harness build)."""
    t0 = time.perf_counter()
    cc = hb.find_cc()
    if cc is None:
        return {'name': 'allocator-selfcheck', 'status': 'SKIP',
                'reason': 'gcc unavailable', 'elapsed': 0.0, 'note': ''}
    tools = hb.build_all(cc)
    selftest = hb.probe_selftest(tools)
    elapsed = time.perf_counter() - t0
    status = 'PASS' if selftest.get('ok') else 'FAIL'
    note = 'guard exit=%s canary exit=%s' % (
        selftest.get('guard', {}).get('exit'),
        selftest.get('canary', {}).get('exit'))
    return {'name': 'allocator-selfcheck', 'status': status,
            'elapsed': elapsed, 'note': note,
            'tail': '' if status == 'PASS' else repr(selftest), 'exit': 0}


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--fast', action='store_true',
                    help='quick pass (smaller fuzz counts)')
    ap.add_argument('--only', default=None,
                    help='comma-separated step name substrings to run')
    ap.add_argument('--exclude', default=None,
                    help='comma-separated step name substrings to skip')
    ap.add_argument('--list', action='store_true', help='list steps and exit')
    ap.add_argument('--keep', action='store_true',
                    help='keep intermediate mutant/audio files')
    ap.add_argument('--json', default=os.path.join(OUT, 'run_all.json'))
    args = ap.parse_args(argv)

    os.makedirs(TMP, exist_ok=True)
    steps = build_steps(args)

    def selected(step):
        if args.only:
            want = [s.strip() for s in args.only.split(',') if s.strip()]
            if not any(w.lower() in step.name.lower() for w in want):
                return False
        if args.exclude:
            drop = [s.strip() for s in args.exclude.split(',') if s.strip()]
            if any(w.lower() in step.name.lower() for w in drop):
                return False
        return True

    steps = [s for s in steps if selected(s)]
    if args.list:
        for s in steps:
            print('%-32s %s' % (s.name, s.skip_reason or 'ready'))
        return 0
    if not steps:
        log('run_all: no steps selected')
        return 2

    log('=' * 78)
    log('FVP robustness suite (%d steps%s)' %
        (len(steps), ', fast mode' if args.fast else ''))
    log('=' * 78)
    rows = []
    t_start = time.perf_counter()
    for idx, step in enumerate(steps, 1):
        log('')
        log('[%d/%d] %s' % (idx, len(steps), step.name))
        if step.name == 'allocator-selfcheck':
            row = run_selfcheck()
        else:
            row = run_step(step)
        if row['status'] == 'SKIP':
            log('  SKIP  %s' % row.get('reason', ''))
        elif row['status'] == 'PASS':
            log('  PASS  %.1f s  %s' % (row['elapsed'], row.get('note', '')))
        else:
            log('  %s  %.1f s' % (row['status'], row['elapsed']))
            if row.get('tail'):
                log(tail(row['tail'], 30))
        rows.append(row)

    elapsed = time.perf_counter() - t_start
    log('')
    log('=' * 78)
    log('SUMMARY')
    log('=' * 78)
    log('  %-32s %-8s %9s  %s' % ('step', 'status', 'time', 'note'))
    for row in rows:
        log('  %-32s %-8s %8.1fs  %s' %
            (row['name'], row['status'], row['elapsed'], row.get('note', '')))
    passed = sum(1 for r in rows if r['status'] == 'PASS')
    failed = sum(1 for r in rows if r['status'] in ('FAIL', 'TIMEOUT', 'ERROR'))
    skipped = sum(1 for r in rows if r['status'] == 'SKIP')
    log('')
    log('  %d passed, %d failed, %d skipped in %.1f s' %
        (passed, failed, skipped, elapsed))
    verdict = 'PASS' if failed == 0 else 'FAIL'
    log('  OVERALL: %s' % verdict)
    report = {'when': time.strftime('%Y-%m-%d %H:%M:%S'), 'fast': args.fast,
              'rows': [{k: v for k, v in r.items() if k != 'tail'}
                       for r in rows],
              'elapsed_s': elapsed, 'verdict': verdict}
    hb.write_json(args.json, report)
    log('  json -> %s' % args.json)
    return 0 if verdict == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())