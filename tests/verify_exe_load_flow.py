#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""存档/读档引擎侧流程的门禁（Load(40) / SaveWrite(96)）。

事实来源：`scripts/extract-exe-load-flow.py` 从 exe 里读的字节/偏移/调用点，
基线 `ref/exe-load-flow.json`。冻结内容：

* 存档路径格式 `0x45DA64 = "%s/save/s%03d.bin"`（root=engine+0x6A7E64、
  slot=engine+0xFCC）——外壳现在写 `<game_dir>\\save\\sNNN.bin`，路径约定一致；
* `Load(40)` handler 0x43A870：int 0..999；写 engine+0xFCB=1、0xFCC=slot、
  线程对象 +0x814=1；每帧由 0x43F876 消费 → 0x43EE90（真正读档）→ 线程 +0xFD9=1；
* `SaveWrite(96)` handler 0x43C1E0：int 0..999；写 0xFCA=1、0xFCC=slot；
  engine+0xFC9!=0 时立即调 0x438540；
* 线程重启：+0xFD9 置位时本帧跳过线程执行、engine+0x6A66D0 归零 →
  下一帧线程从入口重跑（靠回来的全局变量回场景）；
* 两作脚本里 Load 9 个调用点、SaveWrite 3 个、HistorySet 7 个（数字冻结）。

还没逆清的两条（基线里也写着）：0x4344C0 的字段映射、线程重跑后脚本靠哪个
全局量选场景。缺 exe 时 SKIP。

用法：python tests/verify_exe_load_flow.py
"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-load-flow.py')
REF = os.path.join(FVP, 'ref', 'exe-load-flow.json')
EXE = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                   'iroseka_HD.exe')
DOC = os.path.join(FVP, 'docs', 'save-format.md')
SHELL = os.path.join(FVP, 'src', 'platform', 'win32', 'main_win32.c')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def main():
    if not os.path.isfile(EXE):
        print('SKIP: 缺 exe %s' % EXE)
        return 0
    for p in (EXTRACT, REF, DOC, SHELL):
        if not os.path.isfile(p):
            print('FAIL: 缺 %s' % p)
            return 1

    print('== 1. 重新提取并与基线逐字节比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', EXTRACT, '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=300)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0, 'extract-exe-load-flow.py --check 退出码 0')

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)

    print('== 2. 存档路径约定 ==')
    sp = ref['save_path']
    check(sp['format'] == '%s/save/s%03d.bin',
          '路径格式 = %%s/save/s%%03d.bin（实际 %r）' % (sp['format'],))
    check(sp['root_field'] == 'engine+0x6a7e64', '根目录字段 engine+0x6a7e64')
    check(sp['slot_field'] == 'engine+0xfcc', '槽号字段 engine+0xfcc')
    shell = io.open(SHELL, encoding='utf-8').read()
    check('"save\\\\s*.bin"' in shell or 'save\\\\s*.bin' in shell,
          '外壳扫描 <game_dir>\\save\\s*.bin')
    check('L"\\\\s%03d.bin"' in shell,
          '外壳写 <game_dir>\\save\\sNNN.bin')

    print('== 3. Load(40) handler ==')
    L = ref['load']
    check(L['handler'] == '0x0043a870', 'handler 0x43A870')
    check(L['arg_range'] == [0, 999], '实参范围 0..999')
    check(L['bytes'].startswith('8b442404'), 'handler 起始字节冻结')
    fields = [w['field'] for w in L['writes']]
    check(fields == ['engine+0xfcb', 'engine+0xfcc', 'thread+0x814'],
          '写入字段顺序 %r' % (fields,))
    check(L['writes'][0]['value'] == 1, 'engine+0xfcb = 1（读档请求）')

    print('== 4. SaveWrite(96) handler ==')
    W = ref['save_write']
    check(W['handler'] == '0x0043c1e0', 'handler 0x43C1E0')
    check(W['arg_range'] == [0, 999], '实参范围 0..999')
    wf = [w['field'] for w in W['writes']]
    check(wf == ['engine+0xfca', 'engine+0xfcc'], '写入字段 %r' % (wf,))
    check(W['immediate_call']['when'] == 'engine+0xfc9 != 0',
          'engine+0xfc9 != 0 时立即写缩略图/索引缓存')

    print('== 5. 每帧消费与线程重启 ==')
    fc = ref['frame_consumers']
    check(fc['save']['pc'] == '0x0043f7e6', '存档消费点 0x43F7E6')
    check(fc['load']['pc'] == '0x0043f876', '读档消费点 0x43F876')
    check(fc['load_impl']['fn'] == '0x0043ee90', '真正读档函数 0x43EE90')
    check('0x4344c0' in fc['load_impl']['sequence'][1].lower() or
          '0x4344c0' in str(fc['load_impl']['sequence']).lower(),
          '读档流程里调用 0x4344C0（应用存档）')
    tr = fc['thread_restart']
    check(tr['flag'] == 'thread+0xfd9', '线程重启标志 thread+0xFD9')
    check(len(tr['checks']) == 3, '3 处检查 +0xFD9')
    check('跳过' in tr['behavior'] and '重跑' in tr['behavior'],
          '行为写明"本帧跳过线程、下一帧重跑"')

    print('== 5b. 槽文件读取助手与复位序列 ==')
    fio = fc['file_io_helper']
    check(fio['fn'] == '0x0043c4c0', '文件读取助手 0x4344C0')
    check(fio['bytes'].startswith('51538b5c24'), '助手起始字节冻结')
    check(len(fio['iat_calls']) >= 4, '至少 4 个 IAT 调用（CreateFile/Size/Read/Close）')
    check('0x0045d070' in fio['iat_calls'], 'IAT 0x45D070（CreateFileW）')
    check(fio['buffer_fields'] == ['engine+0x0', 'engine+0x4（基址/长度）'],
          '整文件缓冲写 engine+0/+4')
    check('失敗' in fio['error_string'] or '失败' in fio['error_string'],
          '错误串是 CP932 的"打开失败"（%r）' % fio['error_string'][:20])
    seq = fc['load_impl']['sequence']
    joined = ' / '.join(seq)
    check('0x438d20' in joined and '场景/图元' in joined,
          '复位序列里 0x438D20 = 场景/图元树复位')
    check('0x441350' in joined and '脚本引擎复位' in joined,
          '复位序列里 0x441350 = 脚本引擎复位（重启线程）')

    print('== 5c. 归档应用（0x43EE90 的流式反序列化）==')
    la = fc.get('load_apply') or {}
    check(la.get('fn') == '0x0043ee90', '应用点在 0x43EE90')
    check(la.get('file_read_fn') == '0x0043c4c0',
          '读文件助手是 0x4344C0（只读文件，不改字段）')
    check('读' in fc['load_impl']['sequence'][1] and
          '应用存档' not in fc['load_impl']['sequence'][1],
          'load_impl 的序列把 0x4344C0 记成"读文件"而不是"应用存档"')
    check('CreateFileW' in ' '.join(la.get('file_read_calls') or []),
          '读文件助手用 CreateFileW/GetFileSize/ReadFile/CloseHandle')
    segs = la.get('segments') or []
    check(len(segs) >= 2, '至少两段（%d）' % len(segs))
    cont = [x.get('container') for x in segs]
    check('engine+0xfb4' in cont and 'engine+0xfb8' in cont,
          '两段容器是 engine+0xFB4 / 0xFB8（实际 %r）' % (cont,))
    for seg in segs[:2]:
        check(seg.get('push_fn') == '0x00433c60' and
              seg.get('alloc_fn') == '0x004470eb' and
              seg.get('memcpy_fn') == '0x00447800',
              '%s 段：alloc 0x4470EB → memcpy 0x447800 → push 0x433C60'
              % seg.get('container'))
    check(la.get('cursor_field') == 'engine+0x8', '流游标是 engine+0x8')
    unk = ' '.join(ref.get('unknown') or [])
    check('0x4344c0 应用存档' not in unk,
          'unknown 里不再把 0x4344C0 说成"应用存档"')
    check('反序列化' in unk or '流式' in unk,
          'unknown 改成"流式反序列化各段对应哪些状态"')

    print('== 6. 脚本调用点（两作数字冻结）==')
    for g in ('iroseka', 'hoshimemo'):
        s = ref['script_sites'][g]
        check(len(s['Load']) == 9, '%s: Load 9 个调用点（实际 %d）'
              % (g, len(s['Load'])))
        check(len(s['SaveWrite']) == 3, '%s: SaveWrite 3 个（实际 %d）'
              % (g, len(s['SaveWrite'])))
        check(len(s['HistorySet']) == 7, '%s: HistorySet 7 个（实际 %d）'
              % (g, len(s['HistorySet'])))
        slots = [h['slot'] for h in s['Load']]
        check(901 in slots and 902 in slots and 900 in slots and 800 in slots,
              '%s: Load 用到 901/902/900/800 槽（%r）' % (g, slots))
    check(len(ref['unknown']) >= 2,
          '基线里保留"还没逆清"的条目（%d 条）' % len(ref['unknown']))

    print('== 7. 文档记录 ==')
    doc = io.open(DOC, encoding='utf-8').read()
    check('Load(40)' in doc and 'SaveWrite(96)' in doc,
          'save-format 记录 Load(40)/SaveWrite(96) 的引擎侧流程')
    check('s%03d.bin' in doc or 'sNNN.bin' in doc, '文档写了槽文件名约定')
    check('thread+0xfd9' in doc.lower() or '0xFD9' in doc,
          '文档写了线程重启标志')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: Load(40)/SaveWrite(96) 引擎侧流程冻结（路径/请求字段/消费点/'
          '线程重启），字段映射与场景选择仍列为未知')
    return 0


if __name__ == '__main__':
    sys.exit(main())