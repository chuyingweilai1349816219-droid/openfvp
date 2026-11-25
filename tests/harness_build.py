#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""harness_build.py -- shared build helpers for the fuzz/robustness tests.

The bundled w64devkit gcc has no libasan/libubsan, so the harnesses link the
fuzz-only allocator from tests/fuzz_alloc.h instead (heap canary or page guard;
see that file and fvp/docs/testing.md).  Nothing under fvp/src, the Makefile or
the existing tests is modified: every artefact is written to fvp/out/fuzz/.

Used by tests/fuzz_hcb.py and tests/test_vm_edge.py (and via them by
tests/run_all.py).
"""
import glob
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
SRC = os.path.join(FVP, 'src')
TESTS = os.path.join(HERE)
OUT = os.path.join(FVP, 'out', 'fuzz')

CORE_SRC = [os.path.join(SRC, n) for n in
            ('fvp.c', 'encoding.c', 'hcb.c', 'vm.c', 'syscall.c', 'flags.c',
             'textattr.c', 'save.c', 'save_vm.c', 'thumb.c', 'motion.c',
             'timer.c', 'history.c')]
CORE_HDR = [os.path.join(SRC, n) for n in
            ('fvp.h', 'encoding.h', 'hcb.h', 'vm.h', 'syscall.h',
             'flags.h', 'textattr.h', 'save.h', 'save_vm.h', 'thumb.h',
             'motion.h', 'timer.h', 'history.h', 'gen_cmd_table.h',
             'gen_cp932.h', 'gen_gbk.h')]
ALLOC = os.path.join(TESTS, 'fuzz_alloc.h')

BASE_FLAGS = ['-std=c99', '-O1', '-g', '-Wall', '-Wextra',
              '-Wno-unused-function', '-I', SRC, '-I', TESTS]


def log(msg):
    print(msg)
    sys.stdout.flush()


def find_cc(explicit=None):
    """Return the gcc command as a list, or None."""
    if explicit:
        return [explicit]
    root = os.path.dirname(FVP)
    candidates = [os.path.join(root, 'tools', 'w64devkit', 'w64devkit', 'bin',
                               'gcc.exe')]
    import shutil
    for name in ('gcc.exe', 'gcc'):
        found = shutil.which(name)
        if found:
            candidates.append(found)
    for cand in candidates:
        if os.path.isfile(cand):
            return [cand]
    return None


def newest(paths):
    stamp = 0.0
    for path in paths:
        try:
            stamp = max(stamp, os.path.getmtime(path))
        except OSError:
            pass
    return stamp


def build_env():
    """Environment for the compiler.

    On this host the sandbox's temp directory may not be writable by child
    processes (gcc needs a scratch directory for assembler output), so point
    TMP/TEMP at a directory inside the workspace before invoking gcc.
    """
    env = os.environ.copy()
    tmpdir = os.path.join(OUT, 'tmp', 'gcctmp')
    try:
        os.makedirs(tmpdir, exist_ok=True)
    except OSError:
        return env
    env['TMP'] = tmpdir
    env['TEMP'] = tmpdir
    env['TMPDIR'] = tmpdir
    return env


def _run_build(cc, out, sources, defines, flags, libs, with_sp):
    cmd = list(cc) + BASE_FLAGS + list(defines)
    if with_sp:
        cmd += ['-fstack-protector-all']
    cmd += list(flags) + ['-o', out] + list(sources) + list(libs)
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          env=build_env())
    return proc


def build(cc, out_name, sources, deps=None, defines=(), flags=(), libs=('-lm',),
          force=False):
    """Compile out_name if missing/out of date.  Returns the path or None.

    Tries -fstack-protector-all first; silently retries without it when the
    toolchain has no libssp.
    """
    os.makedirs(OUT, exist_ok=True)
    out = os.path.join(OUT, out_name)
    dep_list = list(deps if deps is not None else sources) + [ALLOC]
    if not force and os.path.isfile(out) and os.path.getmtime(out) >= newest(dep_list):
        return out
    log('building %s ...' % out_name)
    proc = _run_build(cc, out, sources, defines, flags, libs, True)
    if proc.returncode != 0:
        proc = _run_build(cc, out, sources, defines, flags, libs, False)
    if proc.returncode != 0:
        log('  FAILED to build %s' % out_name)
        tail = proc.stderr.decode('utf-8', 'replace')[-2500:]
        if tail.strip():
            log(tail)
        return None
    return out


def public_tools():
    """Paths of the prebuilt project executables (read-only artefacts)."""
    return {
        'fvm': os.path.join(FVP, 'bin', 'fvm.exe'),
        'c_assets': os.path.join(FVP, 'bin', 'c_assets.exe'),
        'test_snd': os.path.join(FVP, 'bin', 'test_snd.exe'),
        'test_save': os.path.join(FVP, 'bin', 'test_save.exe'),
        'test_save_vm': os.path.join(FVP, 'bin', 'test_save_vm.exe'),
        'test_flags': os.path.join(FVP, 'bin', 'test_flags.exe'),
        'test_textattr': os.path.join(FVP, 'bin', 'test_textattr.exe'),
        'test_thumb': os.path.join(FVP, 'bin', 'test_thumb.exe'),
        'test_encode': os.path.join(FVP, 'bin', 'test_encode.exe'),
        'test_wheel': os.path.join(FVP, 'bin', 'test_wheel.exe'),
        'test_motion': os.path.join(FVP, 'bin', 'test_motion.exe'),
        'test_gfx_xform': os.path.join(FVP, 'bin', 'test_gfx_xform.exe'),
        'test_titleenter': os.path.join(FVP, 'bin',
                                           'test_titleenter.exe'),
        'test_titlebg': os.path.join(FVP, 'bin', 'test_titlebg.exe'),
        'test_control': os.path.join(FVP, 'bin', 'test_control.exe'),
        'test_graphcolor': os.path.join(FVP, 'bin', 'test_graphcolor.exe'),
        'test_textbuff': os.path.join(FVP, 'bin', 'test_textbuff.exe'),
        'test_gaiji': os.path.join(FVP, 'bin', 'test_gaiji.exe'),
        'test_gaiji_render': os.path.join(FVP, 'bin',
                                          'test_gaiji_render.exe'),
        'test_vmmisc': os.path.join(FVP, 'bin', 'test_vmmisc.exe'),
        'test_nullvalue': os.path.join(FVP, 'bin', 'test_nullvalue.exe'),
        'test_titlemenu': os.path.join(FVP, 'bin', 'test_titlemenu.exe'),
        # 原版 GDI 绘制协议（cp932 + GGO_BITMAP）；Windows 专属，单独 make 目标
        'test_gdi_ggo': os.path.join(FVP, 'bin', 'test_gdi_ggo.exe'),
        'test_savelist': os.path.join(FVP, 'bin', 'test_savelist.exe'),
        'test_openseq': os.path.join(FVP, 'bin', 'test_openseq.exe'),
        'test_titlemenu_draw': os.path.join(FVP, 'bin',
                                           'test_titlemenu_draw.exe'),
        'test_gfx_motion': os.path.join(FVP, 'bin', 'test_gfx_motion.exe'),
        'test_tex_cache': os.path.join(FVP, 'bin', 'test_tex_cache.exe'),
        'test_timer': os.path.join(FVP, 'bin', 'test_timer.exe'),
        'test_typewriter': os.path.join(FVP, 'bin', 'test_typewriter.exe'),
        'test_history': os.path.join(FVP, 'bin', 'test_history.exe'),
        'test_prim_hit': os.path.join(FVP, 'bin', 'test_prim_hit.exe'),
        'test_glyph_fallback': os.path.join(FVP, 'bin', 'test_glyph_fallback.exe'),
    }


def build_all(cc, force=False):
    """Build every harness the robustness suite needs.  Returns a dict."""
    tools = {}
    fuzz_hcb_src = os.path.join(TESTS, 'fuzz_hcb.c')
    fuzz_hcb_deps = [fuzz_hcb_src] + CORE_SRC + CORE_HDR
    tools['fuzz_hcb'] = build(cc, 'fuzz_hcb.exe', [fuzz_hcb_src] + CORE_SRC,
                              deps=fuzz_hcb_deps, force=force)
    tools['fuzz_hcb_guard'] = build(
        cc, 'fuzz_hcb_guard.exe', [fuzz_hcb_src] + CORE_SRC,
        deps=fuzz_hcb_deps, defines=('-DFUZZ_ALLOC_GUARD',), force=force)

    link = os.path.join(TESTS, 'fuzz_alloc_link.c')
    main_src = os.path.join(SRC, 'main.c')
    fvm_deps = [link, main_src] + CORE_SRC + CORE_HDR
    tools['fvm_guard'] = build(cc, 'fvm_guard.exe', [link, main_src] + CORE_SRC,
                               deps=fvm_deps, defines=('-DFUZZ_ALLOC_GUARD',),
                               force=force)

    snd_src = os.path.join(TESTS, 'fuzz_snd.c')
    snd_deps = [snd_src, os.path.join(SRC, 'snd.c'),
                os.path.join(SRC, 'snd.h'),
                os.path.join(SRC, 'third_party', 'stb_vorbis.c')]
    tools['fuzz_snd'] = build(cc, 'fuzz_snd.exe', [snd_src,
                                                   os.path.join(SRC, 'snd.c')],
                              deps=snd_deps, defines=('-DFUZZ_ALLOC_GUARD',),
                              force=force)

    ca_src = os.path.join(TESTS, 'c_assets.c')
    ca_deps = [link, ca_src, os.path.join(SRC, 'bin.c'),
               os.path.join(SRC, 'hzc1.c'), os.path.join(SRC, 'inflate.c'),
               os.path.join(SRC, 'bin.h'), os.path.join(SRC, 'hzc1.h'),
               os.path.join(SRC, 'inflate.h')]
    tools['c_assets_guard'] = build(
        cc, 'c_assets_guard.exe',
        [link, ca_src, os.path.join(SRC, 'bin.c'),
         os.path.join(SRC, 'hzc1.c'), os.path.join(SRC, 'inflate.c')],
        deps=ca_deps, defines=('-DFUZZ_ALLOC_GUARD',), force=force)

    # Self-test of the allocator: proves the page-guard / canary detector
    # actually fires (otherwise "0 faults" would be meaningless).  Built at
    # -O0 -fno-builtin: at -O1 gcc is allowed to treat malloc/free specially
    # and would delete the deliberate one-byte overflow as a dead store.
    selftest = os.path.join(TESTS, 'fuzz_selftest.c')
    selftest_flags = ('-O0', '-fno-builtin')
    if os.path.isfile(selftest):
        tools['selftest_guard'] = build(
            cc, 'fuzz_selftest_guard.exe', [selftest],
            deps=[selftest], defines=('-DFUZZ_ALLOC_GUARD',),
            flags=selftest_flags, force=force)
        tools['selftest_canary'] = build(
            cc, 'fuzz_selftest_canary.exe', [selftest], deps=[selftest],
            flags=selftest_flags, force=force)
    return tools


def probe_selftest(tools):
    """Run the allocator self-test binaries; returns a dict describing them.

    Guard build should die with an access violation (negative exit code, or 71
    when a harness SEH handler is installed) because the write lands in the
    guard page.  Canary build should exit 72 (trailer mismatch on free).
    """
    result = {'guard': None, 'canary': None, 'ok': False}
    for key, expect in (('guard', 'nonzero'), ('canary', 72)):
        exe = tools.get('selftest_%s' % key)
        if not exe or not os.path.isfile(exe):
            continue
        proc = subprocess.run([exe], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=60)
        rc = proc.returncode
        if expect == 'nonzero':
            detected = rc != 0
        else:
            detected = rc == expect
        result[key] = {'exit': rc, 'expected': expect,
                       'detected': detected,
                       'stderr': proc.stderr.decode('utf-8',
                                                    'replace')[-300:]}
    result['ok'] = bool(result['guard'] and result['guard']['detected'] and
                        result['canary'] and result['canary']['detected'])
    return result


def write_json(path, payload):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(payload, f, indent=1, ensure_ascii=False)