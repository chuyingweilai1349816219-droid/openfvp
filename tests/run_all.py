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

    # 6.55 游戏内菜单的三个动作（SAVE/LOAD/OPTION）
    vma = os.path.join(HERE, 'verify_menu_actions.py')
    steps.append(Step('verify_menu_actions.py', [PY, vma], 900,
                      skip_reason=None,
                      note='menu SAVE writes a template slot, LOAD opens the list, OPTION panel renders'))

    # 6.5 游戏内菜单（引擎侧 UI，--menu-at 打开）
    vg = os.path.join(HERE, 'verify_gamemenu.py')
    steps.append(Step('verify_gamemenu.py', [PY, vg], 900,
                      skip_reason=None,
                      note='in-game menu overlay: opens, 5 items, ink present'))

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