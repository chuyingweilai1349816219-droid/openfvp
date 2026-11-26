#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 原版程序里冻结「台词历史记录」的全部事实（A 类，非拟合）。

为什么：`HistorySet(26)` / `HistoryGet(25)` 的存储层语义此前只写在
`src/history.h` 的注释里（人工读出来的），没有基线也没有门禁；而且
"count 初值" 和 "begin 的判据" 两条容易读错。本脚本把**每一条判据的地址 +
指令字节**、常量（100 / 0xC / 0xB28 / 0x814）、数据来源偏移，以及两作脚本里
的 HistorySet/HistoryGet 调用点形状，一起冻结进 `ref/exe-history.json`。

产物：`ref/exe-history.json`
门禁：`tests/verify_exe_history.py`（还会拿 6 份真实存档解析槽尾的历史环）

用法：
    python scripts/extract-exe-history.py            # 打印 + 写 json
    python scripts/extract-exe-history.py --check    # 只比对（门禁用）
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
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-history.json')
BASE = 0x400000

GAMES = {
    'iroseka': os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                            'iroseka.hcb'),
    'hoshimemo': os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed',
                              'Hoshimemo_HD.hcb'),
}

# 每条判据：地址 + 必须出现的指令字节（都从原版程序里抄的，不是猜的）
CHECKS = [
    # --- 容器布局与构造（engine+0x6A49C0） ---
    {'id': 'ctor_count_1', 'va': '0x00442c4a', 'frags': ['c70601000000'],
     'fact': '构造时 count = 1（`movl $0x1,(%esi)`）'},
    {'id': 'ctor_pos_0', 'va': '0x00442c0c', 'frags': ['897e04'],
     'fact': '构造时 pos = 0（`mov %edi,0x4(%esi)`，edi 已被 xor 清零）'},
    {'id': 'ctor_ring_new', 'va': '0x00442c07', 'frags': ['68b4040000', '6a64', '6a0c'],
     'fact': '记录环 = new 0x4B4 字节（100 × 12 + 4 的头），数组头写入 0x64=100'},
    {'id': 'ctor_base_store', 'va': '0x00442c40', 'frags': ['897e08'],
     'fact': 'base（记录数组 +4 之后）写进 container+8'},
    # --- 序列化（存档槽尾那一段） ---
    {'id': 'ser_count', 'va': '0x00438154', 'frags': ['8b03', '8902'],
     'fact': '序列化先写 u32 count（取 container+0）'},
    {'id': 'ser_pos', 'va': '0x00438181', 'frags': ['8b5304', '8911'],
     'fact': '再写 u32 pos（取 container+4）'},
    {'id': 'ser_ring', 'va': '0x00438190',
     'frags': ['8b4b08', 'e805feffff', '83c70c', '81ffb0040000'],
     'fact': '然后 100 次（0x4B0 / 0xC）调用 0x437FA0 写每条记录'},
    {'id': 'rec_writer_s0', 'va': '0x00437fa4',
     'frags': ['8b03', '8b4e08', '83c104'],
     'fact': '记录写入 0x437FA0：先 u32 strlen(text0)（0 就不写字节）'},
    {'id': 'rec_writer_s1', 'va': '0x00437ffb', 'frags': ['8b4304'],
     'frags2': ['8b4e08', '83c104'], 'va2': '0x00438012',
     'fact': '再 u32 strlen(text1)（0 就不写字节）'},
    {'id': 'rec_writer_value', 'va': '0x00438071', 'frags': ['8b4308', '5f', '8902'],
     'fact': '最后把 +8 的 i32（value）**原样**写 4 字节'},
    # --- 反序列化（读档路径，紧跟在 0x400 调色板之后） ---
    {'id': 'deser_count', 'va': '0x0043f262', 'frags': ['8b02', '8986c0496a00'],
     'fact': '读档：u32 → engine+0x6A49C0（count）'},
    {'id': 'deser_pos', 'va': '0x0043f272', 'frags': ['8b1401', '8996c4496a00'],
     'fact': '读档：u32 → engine+0x6A49C4（pos）'},
    {'id': 'deser_ring', 'va': '0x0043f282',
     'frags': ['8b8ec8496a00', 'e8508cffff', '83c70c', '81ffb0040000'],
     'fact': '读档：100 次（0x4B0/0xC）调用 0x437EE0 读每条记录'},
    {'id': 'rec_reader', 'va': '0x00437ee0',
     'frags': ['8b07', '85c0', 'e880040100'],
     'fact': '记录读取 0x437EE0：先释放旧串（0x448371=free），再按 len 分配 len+1'},
    # --- HistorySet(26) handler 0x43A5E0 ---
    {'id': 'set_null_gate', 'va': '0x0043a5e0', 'frags': ['8b442404', '8a10', '84d2'],
     'fact': 'HistorySet：arg0 的第一字节（Variant type）为 0 才是 begin'},
    {'id': 'set_begin', 'va': '0x0043a5ea',
     'frags': ['8b81c0496a00', '83f864', '7d07', '40', '8981c0496a00'],
     'fact': 'begin：count < 0x64 才自增（上限 100）'},
    {'id': 'set_pos_wrap', 'va': '0x0043a5fc',
     'frags': ['ff81c4496a00', '83b9c4496a0064', 'c781c4496a0000000000'],
     'fact': 'begin：pos++，到 100 归零（先 ++ 再写 → 第一条落在槽 0 需 pos 初值 0）'},
    {'id': 'set_clear', 'va': '0x0043a627',
     'frags': ['c70000000000', 'c7400400000000', 'c74008ffffffff'],
     'fact': 'begin：新记录清成 {NULL, NULL, -1}'},
    {'id': 'set_kind_gate', 'va': '0x0043a63e',
     'frags': ['80fa02', '0f858d000000', '8b5004', '85d2'],
     'fact': 'arg0 必须是 Variant type 2（int），否则整条忽略'},
    {'id': 'set_kind_range', 'va': '0x0043a652', 'frags': ['83fa02', '7f7d'],
     'fact': 'kind 必须 0..2'},
    {'id': 'set_kind2_gate', 'va': '0x0043a6b0',
     'frags': ['83fa02', '751f', '385008', '751a', '8b400c', '85c0', '7c13'],
     'fact': 'kind 2：arg1 必须是 int（type 2）且 >= 0，才写 record+8'},
    {'id': 'set_text_writers', 'va': '0x0043a67e',
     'frags': ['e9add7ffff'], 'fact': 'kind 0 写 record+0（跳 0x437E30）'},
    {'id': 'set_text_writers2', 'va': '0x0043a6ab',
     'frags': ['e9d0d7ffff'], 'fact': 'kind 1 写 record+4（跳 0x437E80）'},
    # --- HistoryGet(25) handler 0x43A490 ---
    {'id': 'get_thread', 'va': '0x0043a490',
     'frags': ['568bf1', '8b86d0666a00', '69c0280b0000'],
     'fact': '返回槽 = engine+0x68DFC0 + engine[0x6A66D0] * 0xB28（线程对象的 +0x814/+0x818）'},
    {'id': 'get_null_returns_count', 'va': '0x0043a4ab',
     'frags': ['8a08', '84c9', '7518', 'c6871408000002',
               '8b8ec0496a00', '898f18080000'],
     'fact': 'arg0 为 type 0（null）→ 返回值 = count（type 2/int）'},
    {'id': 'get_kind_gate', 'va': '0x0043a4c9',
     'frags': ['80f902', '0f8504010000', '8b4804', '85c9', '0f8cf9000000',
               '83f902', '0f8ff0000000'],
     'fact': 'arg0 必须是 int，kind 必须 0..2'},
    {'id': 'get_arg1_gate', 'va': '0x0043a4e6',
     'frags': ['80780802', '8b500c', '85d2', '0f8cdb000000'],
     'fact': 'arg1 必须是 int（type 2）且 idx >= 0'},
    {'id': 'get_idx_range', 'va': '0x0043a4fc',
     'frags': ['8b9ec0496a00', '8daec0496a00', '4b', '3bd3'],
     'fact': 'idx > count-1 → 不写返回值'},
    {'id': 'get_slot_map', 'va': '0x0043a531',
     'frags': ['53', '8bcd', 'e857dbffff'],
     'fact': '取字段调 0x438090（slot = pos - idx，负数 +100）'},
    {'id': 'get_kind0_string', 'va': '0x0043a518',
     'frags': ['8bcfe8c1a00000', '8db486ec496a00', 'e80f97ffff'],
     'fact': 'kind 0/1：先 0x4445E0 取 128 槽动态字符串句柄、0x433C40 清空、0x433C60 追加（未设置 → 空串）'},
    {'id': 'slot_map_body', 'va': '0x00438090',
     'frags': ['8b542404', '3b11', '7c05', '33c0', '8b4104', '2bc2', '83c064'],
     'fact': '0x438090：slot = pos - idx；< 0 时 +0x64（100）'},
]

# 数据侧证据（详见门禁：它会在 6 份真实存档里跑）
DATA_EVIDENCE = [
    {'id': 'ring_in_slot_tail',
     'claim': '槽尾里有历史环的序列化：u32 count + u32 pos + 100 条 '
              '{u32 len0, text0, u32 len1, text1, i32 value}',
     'sample': 's481.bin 环 @tail+0x20622（count=1 pos=0，1 条 = 当前行）；'
               's905.bin 环 @tail+0x20633（count=11 pos=10，11 条台词，'
               '含"　从天而降的羽毛。"等）'},
    {'id': 'count_semantics',
     'claim': 'pos == count-1（未回绕时）且 count=1 表示"只有当前这一行"',
     'sample': 's481: count=1/pos=0；s905: count=11/pos=10 —— 与'
               '"构造 count=1、begin 先 ++ 再写"完全一致'},
    {'id': 'line_matches_header',
     'claim': '最新一条（槽 pos）的 text1 == 槽头第 3 条字符串（当前台词行）',
     'sample': 's481/s905 都成立；脚本从没发过 null，但存档里 count>1 ⇒ '
               'begin 的真实触发源仍未解'},
]

UNKNOWN = [
    'begin（arg0 = Variant type 0）的真实触发源：静态扫描两作各只有 7 个 '
    'HistorySet 调用点，arg0 全是 int/bool，看不到 null；但存档里 count 会到 11'
    '（10 次 begin）⇒ 引擎内部某处调了这个 handler（或在换行时直接写 container），'
    '还没找到那个调用点',
    'HistoryGet 在两作里动态调用次数都是 0（脚本只压参数不调），所以读档菜单'
    '（回想）到底走哪条路径仍未知',
    '读档时 count/pos 由 0x43F25A 起的一段直接写回 container，但"读回来之后'
    '当前行与文本窗口如何对齐"未解',
]

PUSH_TYPES = {
    'pushtrue': 'null', 'pushfalse': 'bool', 'pushint32': 'int',
    'pushint16': 'int', 'pushint8': 'int', 'pushfloat': 'float',
    'pushstring': 'str', 'pushglobal': 'global', 'pushstack': 'stack',
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


def check_frags(blob, frags):
    text = blob.hex()
    for frag in frags:
        if frag not in text:
            return False, frag
    return True, None


def script_sites():
    """两作里 HistorySet(26)/HistoryGet(25) 的调用点 + arg0 的静态类型。"""
    out = {}
    for name, path in sorted(GAMES.items()):
        if not os.path.isfile(path):
            out[name] = {'note': 'SKIP: HCB 不存在'}
            continue
        code = fvm_ref.Code(open(path, 'rb').read(), 'cp932')
        order = sorted(code.instr)
        sites = {}
        for cid, label in ((25, 'HistoryGet'), (26, 'HistorySet')):
            hits = []
            for i, pc in enumerate(order):
                op, nm, args = code.instr[pc]
                if nm != 'syscall' or not args or args[0] != cid:
                    continue
                back = []
                for j in range(1, 4):
                    if i - j < 0:
                        break
                    _o, n2, a2 = code.instr[order[i - j]]
                    back.append({'op': n2,
                                 'type': PUSH_TYPES.get(n2, '?')})
                hits.append({'pc': '0x%08x' % pc, 'recent_pushes': back})
            sites[label] = hits
        out[name] = sites
    return out


def extract(path):
    data, secs = load_exe(path)
    checks = []
    for c in CHECKS:
        blob = u8(data, secs, int(c['va'], 16), 0x40)
        ok, bad = check_frags(blob, c['frags'])
        if not ok:
            raise SystemExit('FAIL: %s @%s 判据字节 %s 不符（实际 %s）'
                             % (c['id'], c['va'], bad, blob.hex()))
        rec = {'id': c['id'], 'va': c['va'], 'fact': c['fact'],
               'bytes': blob.hex()}
        if 'va2' in c:
            blob2 = u8(data, secs, int(c['va2'], 16), 0x40)
            ok2, bad2 = check_frags(blob2, c['frags2'])
            if not ok2:
                raise SystemExit('FAIL: %s @%s 判据字节 %s 不符'
                                 % (c['id'], c['va2'], bad2))
            rec['va2'] = c['va2']
            rec['bytes2'] = blob2.hex()
        checks.append(rec)

    return {
        'exe': {'sha256': hashlib.sha256(data).hexdigest(),
                'size': len(data), 'image_base': '0x%08x' % BASE},
        'container': {
            'engine_offset': 'engine+0x6a49c0',
            'fields': {'+0x00': 'u32 count（构造 = 1，上限 100）',
                       '+0x04': 'u32 pos（构造 = 0，写前先 ++，到 100 归零）',
                       '+0x08': '记录数组 base（new 0x4B4，100 × 12B）'},
            'record': {'size': 12,
                       'fields': {'+0x00': 'char* text0（人名）',
                                  '+0x04': 'char* text1（台词正文）',
                                  '+0x08': 'i32 value（行 id，-1 = 未设置）'}},
        },
        'handlers': {
            'HistorySet': {'cmd': 26, 'va': '0x0043a5e0',
                           'begin_gate': 'arg0 的 Variant type == 0（null）',
                           'kind_gate': 'arg0 type == 2（int），kind 0..2',
                           'text_gate': 'kind 0/1 时 arg1 必须是字符串',
                           'value_gate': 'kind 2 时 arg1 必须是 int 且 >= 0'},
            'HistoryGet': {'cmd': 25, 'va': '0x0043a490',
                           'null_returns': 'arg0 type == 0 → 返回 count（int）',
                           'kind_gate': 'arg0 type == 2、kind 0..2、arg1 int '
                                        '且 0 <= idx < count',
                           'slot_map': 'slot = pos - idx（<0 时 +100），'
                                       '见 0x438090',
                           'string_field': 'kind 0/1 返回 128 槽动态字符串'
                                           '表里的句柄（未设置 → 空串）',
                           'thread_result': 'engine+0x68DFC0 + '
                                            'engine[0x6A66D0]*0xB28 的 '
                                            '+0x814（type）/ +0x818（值）'},
        },
        'serializer': {
            'segment': '槽尾第 20 段（0x438120），紧跟 0x400 调色板之后',
            'order': ['u32 count', 'u32 pos',
                      '100 × {u32 strlen(text0), text0?, u32 strlen(text1), '
                      'text1?, i32 value}'],
            'writer': '0x00437FA0', 'reader': '0x00437EE0',
            'load_path': '0x43F25A 起（读档应用序列里紧跟在调色板之后）',
        },
        'checks': checks,
        'data_evidence': DATA_EVIDENCE,
        'script_sites': script_sites(),
        'unknown': UNKNOWN,
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
    print('HistorySet 0x43A5E0 / HistoryGet 0x43A490；判据 %d 条'
          % len(result['checks']))
    for g in sorted(result['script_sites']):
        s = result['script_sites'][g]
        if 'note' in s:
            print('  %s: %s' % (g, s['note']))
            continue
        print('  %s: HistoryGet %d 调用点 / HistorySet %d 调用点'
              % (g, len(s['HistoryGet']), len(s['HistorySet'])))
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        old = io.open(OUT_JSON, encoding='utf-8').read()
        if old != text:
            print('FAIL: 提取结果与 ref/exe-history.json 不一致')
            import difflib
            for line in list(difflib.unified_diff(
                    old.splitlines(), text.splitlines(), 'ref', 'new',
                    lineterm=''))[:40]:
                print(line)
            return 1
        print('OK: 与 ref/exe-history.json 一致')
        return 0
    io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n').write(text)
    print('写入 %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())