#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 里静态提取**存档/读档**的引擎侧流程（Load(40) / SaveWrite(96)）。

为什么：交互闭环里"读档续玩"是硬需求。脚本侧只看到 `Load(slot)` / `SaveWrite(slot)`
两条命令（两作都是 9 / 3 个调用点），真正的读文件、灌全局变量、重启线程都在引擎里。
本脚本把 exe 里可核对的常量/偏移/调用点抄出来冻结（A 类事实），暂不实现：

  * 存档路径格式：`0x45DA64 = "%s/save/s%03d.bin"`（根目录在 engine+0x6A7E64，
    槽号在 engine+0xFCC）——与外壳现在用的 `<game_dir>\\save\\sNNN.bin` 一致；
  * `Load(40)` handler 0x43A870：实参必须 int 且 0..999；写
    engine+0xFCB=1（读档请求）、engine+0xFCC=槽号、当前线程对象 +0x814=1；
  * `SaveWrite(96)` handler 0x43C1E0：实参 int 0..999；写 engine+0xFCA=1、
    engine+0xFCC=槽号；若 engine+0xFC9 != 0 还会立即调 0x438540（写缩略图/索引缓存）；
  * 每帧消费：
    - 存档 0x43F7E6：sprintf 路径 → 0x433FB0 → 0x438300（写文件）→ 0x4396F0；
    - 读档 0x43F876：清 0xFCB → 0x43EE90（sprintf 路径 → **0x4344C0 真正应用存档**
      → 重置 256 个线程对象 → engine+0x101058/0x10105C/0x101060 复位）；
      然后 engine+0x6A7E6C=0、当前线程 +0xFD9=1（**重启请求**）；
    - 重启 0x43F614/0x43F641/0x43F68D：+0xFD9 置位时本帧**跳过脚本线程执行**，
      并把线程下标 engine+0x6A66D0 归零 → 下一帧线程从入口重新跑，靠读回来的
      全局变量回到对应场景。

**还没逆清**：0x4344C0 到底应用存档里的哪些字段（全局变量/flag/文本位图的对应
关系），以及"重新跑线程后脚本用哪个全局量判断该回哪个场景"。

产物 `ref/exe-load-flow.json`。门禁 `tests/verify_exe_load_flow.py`。

用法：
    python scripts/extract-exe-load-flow.py            # 打印 + 写 json
    python scripts/extract-exe-load-flow.py --check    # 只比对（门禁用）
"""
import argparse
import hashlib
import io
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'ref'))
import fvm_ref  # noqa: E402

EXE = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                   'iroseka_HD.exe')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-load-flow.json')
BASE = 0x400000

GAMES = {
    'iroseka': os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                            'iroseka.hcb'),
    'hoshimemo': os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed',
                              'Hoshimemo_HD.hcb'),
}


def load_exe(path):
    data = open(path, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    opt = pe + 24
    sec_off = opt + struct.unpack_from('<H', data, pe + 20)[0]
    secs = []
    for i in range(nsec):
        off = sec_off + i * 40
        name = data[off:off + 8].rstrip(b'\0').decode('latin1')
        vsize, vaddr, rsize, roff = struct.unpack_from('<IIII', data, off + 8)
        secs.append((name, vaddr, vsize, roff, rsize))
    return data, secs


def foff(secs, va):
    rva = va - BASE
    for _n, vaddr, vsize, roff, rsize in secs:
        if vaddr <= rva < vaddr + max(vsize, rsize):
            return roff + (rva - vaddr)
    raise SystemExit('VA %x not mapped' % va)


def u8(data, secs, va, n):
    fo = foff(secs, va)
    return data[fo:fo + n]


def cstr(data, secs, va, n=128):
    fo = foff(secs, va)
    blob = data[fo:fo + n]
    return blob.split(b'\0')[0].decode('latin1')


def cstr_cp932(data, secs, va, n=128):
    fo = foff(secs, va)
    blob = data[fo:fo + n].split(b'\0')[0]
    return blob.decode('cp932', 'replace')


def script_sites():
    """两作里 Load(40)/SaveWrite(96)/HistorySet(26) 的调用点（pc + 实参）。"""
    import importlib.util

    spec = importlib.util.spec_from_file_location(
        'ete', os.path.join(ROOT, 'scripts', 'extract-title-enter.py'))
    ete = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ete)
    with open(os.path.join(ROOT, 'ref', 'cmd_table.json'),
              encoding='utf-8') as fp:
        argc = {e['id']: e['argc'] for e in json.load(fp)}
    out = {}
    for name, path in sorted(GAMES.items()):
        out[name] = {}
        code = fvm_ref.Code(open(path, 'rb').read(), 'cp932')
        order = sorted(code.instr)
        idx = {pc: i for i, pc in enumerate(order)}
        for cid, label in ((40, 'Load'), (96, 'SaveWrite'), (26, 'HistorySet')):
            hits = []
            for pc in order:
                op, nm, args = code.instr[pc]
                if nm != 'syscall' or not args or args[0] != cid:
                    continue
                try:
                    v = ete.args_n(code, order, idx[pc], lambda a, b: None, 1)
                except Exception:  # noqa: BLE001
                    v = None
                hits.append({'pc': '0x%08x' % pc,
                             'slot': v[0] if v else None})
            out[name][label] = hits
    return out


def extract(path):
    data, secs = load_exe(path)
    return {
        'exe': {'sha256': hashlib.sha256(data).hexdigest(),
                'size': len(data), 'image_base': '0x%08x' % BASE},
        'save_path': {
            'format_va': '0x0045da64',
            'format': cstr(data, secs, 0x45DA64),
            'root_field': 'engine+0x6a7e64',
            'slot_field': 'engine+0xfcc',
            'note': '与外壳现在的 <game_dir>\\save\\sNNN.bin 一致',
        },
        'load': {
            'cmd': 40,
            'handler': '0x0043a870',
            'bytes': u8(data, secs, 0x43A870, 0x3C).hex(),
            'arg_range': [0, 999],
            'arg_type_gate': 'cmpb $0x2,(%eax)（必须是 int）',
            'writes': [
                {'field': 'engine+0xfcb', 'value': 1,
                 'note': '读档请求（每帧被 0x43F876 消费）'},
                {'field': 'engine+0xfcc', 'value': 'slot'},
                {'field': 'thread+0x814',
                 'value': 1,
                 'note': '线程对象=engine+0x68dfc0+idx*0xb28（+0x810+4）'},
            ],
        },
        'save_write': {
            'cmd': 96,
            'handler': '0x0043c1e0',
            'bytes': u8(data, secs, 0x43C1E0, 0x44).hex(),
            'arg_range': [0, 999],
            'writes': [
                {'field': 'engine+0xfca', 'value': 1,
                 'note': '存档请求（每帧被 0x43F7E6 消费）'},
                {'field': 'engine+0xfcc', 'value': 'slot'},
            ],
            'immediate_call': {
                'when': 'engine+0xfc9 != 0',
                'fn': '0x00438540',
                'args': ['engine+0x14', 'slot'],
                'note': '写缩略图/索引缓存（不是存档文件本身）',
            },
        },
        'frame_consumers': {
            'save': {
                'pc': '0x0043f7e6',
                'bytes': u8(data, secs, 0x43F7E6, 0x5A).hex(),
                'sequence': ['sprintf(0x4488b7, fmt, root, slot)',
                             'call 0x433fb0（准备写）',
                             'call 0x438300（写文件）',
                             'call 0x4396f0',
                             'timeGetTime + engine+0x6a7e6c 计时'],
            },
            'load': {
                'pc': '0x0043f876',
                'bytes': u8(data, secs, 0x43F876, 0x28).hex(),
                'sequence': ['清 engine+0xfcb',
                             'call 0x43ee90（真正读档）',
                             'engine+0x6a7e6c = 0',
                             'thread+0xfd9 = 1（重启请求）'],
            },
            'file_io_helper': {
                'fn': '0x0043c4c0',
                'bytes': u8(data, secs, 0x4344C0, 0x30).hex(),
                'iat_calls': {
                    '0x0045d070': 'CreateFileW（失败 → 0xFFFFFFFF）',
                    '0x0045d090': 'GetFileSize',
                    '0x0045d08c': 'ReadFile',
                    '0x0045d058': 'CloseHandle',
                    '0x0045d084/0x0045d080': '映射/解映射（读文件用的辅助）',
                },
                'buffer_fields': ['engine+0x0', 'engine+0x4（基址/长度）'],
                'error_string_va': '0x0045d968',
                'error_string': cstr_cp932(data, secs, 0x45D968),
                'error_fn': '0x004258f0（把错误串格式进日志/消息框）',
                'note': '通用"整文件读进内存"助手；槽文件与 save.bin 都用它',
            },
            'load_impl': {
                'fn': '0x0043ee90',
                'bytes': u8(data, secs, 0x43EE90, 0x80).hex(),
                'sequence': ['sprintf(0x4488b7, fmt, root, slot)',
                             'call 0x4344c0（把槽文件读进 engine+0x0/+0x4 缓冲；'
                             '只读文件，不改任何字段）',
                             '重置 256 个线程对象（0x436270(600) 再 0）',
                             'engine+0x101058=3, +0x10105c=600, +0x101060=0',
                             'call 0x441680(engine+0x120)（文本/渲染复位）',
                             'call 0x42a6c0（gfx 复位）',
                             'call 0x438d20(engine, 0, -1)（场景/图元树复位：'
                             '清 prim 树、重设图元）',
                             'call 0x42c4b0（gfx 复位）',
                             'call 0x441350(engine+0x6a7e34, engine+0x6a7e6c)'
                             '（脚本引擎复位：重启线程用）'],
            },
            'load_apply': {
                'fn': '0x0043ee90',
                'note': '真正应用存档的地方：先把槽文件读进 engine+0x0/+0x4 缓冲'
                        '（0x4344c0），复位 256 个线程对象 / 渲染 / 场景，然后按'
                        '**2 字节长度前缀**逐段把数据读进 engine+0xfb4 / 0xfb8 / … 容器'
                        '（0x433c60 push）；流游标就是 engine+0x8。',
                'file_read_fn': '0x0043c4c0',
                'file_read_calls': ['0x0045d070 CreateFileW',
                                    '0x0045d090 GetFileSize',
                                    '0x0045d08c ReadFile',
                                    '0x0045d058 CloseHandle',
                                    '0x0045d084/0x0045d080 映射/解映射'],
                'segments': [
                    {'container': 'engine+0xfb4',
                     'length_read_pc': '0x0043efd5',
                     'alloc_fn': '0x004470eb',
                     'memcpy_fn': '0x00447800',
                     'push_fn': '0x00433c60',
                     'bytes': u8(data, secs, 0x43EFD5, 0x50).hex()},
                    {'container': 'engine+0xfb8',
                     'length_read_pc': '0x0043f028',
                     'alloc_fn': '0x004470eb',
                     'memcpy_fn': '0x00447800',
                     'push_fn': '0x00433c60',
                     'bytes': u8(data, secs, 0x43F028, 0x50).hex()},
                ],
                'cursor_field': 'engine+0x8',
                'main_steps': [
                    {'pc': '0x0043eed7', 'what': 'call 0x4344c0：读文件进缓冲'},
                    {'pc': '0x0043eef0', 'what': '256 × 0x436270(entry, 0x258) 且 '
                                                 'entry+0x11=0：复位线程对象'},
                    {'pc': '0x0043ef12', 'what': 'engine+0x101058=3、'
                                                 '+0x10105c=0x258、+0x101060=0'},
                    {'pc': '0x0043ef62', 'what': 'call 0x438d20(engine, 0, -1)：'
                                                 '场景/图元树复位'},
                    {'pc': '0x0043ef89', 'what': 'call 0x441350：脚本引擎复位'},
                ],
            },
            'thread_restart': {
                'flag': 'thread+0xfd9',
                'checks': ['0x0043f614', '0x0043f641', '0x0043f68d'],
                'behavior': '置位时本帧跳过脚本线程执行；'
                            'engine+0x6a66d0（线程下标）归零 → 下一帧线程从入口重跑',
            },
        },
        'unknown': [
            '0x43ee90 的流式反序列化**每一段**分别对应哪些状态（已确认按 2 字节长度'
            '串逐段读进 engine+0xfb4/0xfb8/… 容器，但段序与字段的对应还没排全）',
            '线程重跑后脚本用哪个全局量判断"该回哪个场景"',
        ],
        'script_sites': script_sites(),
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)
    if not os.path.isfile(EXE):
        print('SKIP: %s 不存在' % EXE)
        return 0
    result = extract(EXE)
    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    print('save path fmt:', result['save_path']['format'])
    print('Load handler:', result['load']['handler'],
          'range', result['load']['arg_range'])
    print('SaveWrite handler:', result['save_write']['handler'],
          'range', result['save_write']['arg_range'])
    for g in sorted(result['script_sites']):
        s = result['script_sites'][g]
        print('  %s: Load %d / SaveWrite %d / HistorySet %d'
              % (g, len(s['Load']), len(s['SaveWrite']), len(s['HistorySet'])))
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        old = io.open(OUT_JSON, encoding='utf-8').read()
        if old != text:
            print('FAIL: 提取结果与 ref/exe-load-flow.json 不一致')
            import difflib
            for line in list(difflib.unified_diff(
                    old.splitlines(), text.splitlines(), 'ref', 'new',
                    lineterm=''))[:40]:
                print(line)
            return 1
        print('OK: 与 ref/exe-load-flow.json 一致')
        return 0
    io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n').write(text)
    print('写入 %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())