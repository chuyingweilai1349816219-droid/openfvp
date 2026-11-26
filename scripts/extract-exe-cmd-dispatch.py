#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 的命令注册构造函数里抄出 **命令 id → handler 地址**（A 类：ref/exe-cmd-dispatch.json）。

原版在 0x43E000..0x440E00 这段构造函数里逐条注册命令，每条的形态是：

    mov    $0x43XXXX,%ecx      ; handler 地址
    mov    %ecx,(%eax)         ; 填进一条注册记录
    ...
    push   $0xN                ; argc（6a NN / 68 imm32）
    ...
    push   $0x45Exxx           ; 命令名（ASCII 字符串）
    call   0x4387d0            ; register(entry, name)

名字在注册顺序里是**字典序**；命令 id 从 HCB 侧的命令表（ref/cmd_table.json 的
id→name）反查，于是三方连起来就是 **id / 名字 / argc / handler**。

用法：
    python scripts/extract-exe-cmd-dispatch.py            # 打印 + 写 json
    python scripts/extract-exe-cmd-dispatch.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                   'iroseka_HD.exe')
CMD_TABLE = os.path.join(ROOT, 'ref', 'cmd_table.json')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-cmd-dispatch.json')

IMAGE_BASE = 0x400000
LO, HI = 0x43E000, 0x440E00


def sections(buf):
    pe = struct.unpack_from('<I', buf, 0x3C)[0]
    nsec = struct.unpack_from('<H', buf, pe + 6)[0]
    optsize = struct.unpack_from('<H', buf, pe + 20)[0]
    out = []
    for i in range(nsec):
        s = pe + 24 + optsize + i * 40
        name = buf[s:s + 8].rstrip(b'\x00').decode('latin1')
        vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', buf, s + 8)
        out.append((name, vaddr, vsize, rawptr, rawsize))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)
    if not os.path.isfile(EXE):
        print('SKIP: %s 不存在' % EXE)
        return 0
    buf = io.open(EXE, 'rb').read()
    secs = sections(buf)

    def va_to_off(va):
        rva = va - IMAGE_BASE
        for name, vaddr, vsize, rawptr, rawsize in secs:
            if vaddr <= rva < vaddr + vsize:
                return rawptr + (rva - vaddr)
        return None

    def cstr(va, limit=48):
        off = va_to_off(va)
        if off is None:
            return None
        end = buf.find(b'\x00', off, off + limit)
        if end < 0:
            return None
        try:
            return buf[off:end].decode('ascii')
        except UnicodeDecodeError:
            return None

    off_lo, off_hi = va_to_off(LO), va_to_off(HI)
    entries = []
    i = off_lo
    while i < off_hi - 44:
        if buf[i] == 0xB9 and buf[i + 5] == 0x89 and buf[i + 6] == 0x08:
            handler = struct.unpack_from('<I', buf, i + 1)[0]
            argc = None
            name_va = None
            for k in range(i, i + 46):
                if buf[k] == 0x6A:
                    v = buf[k + 1]
                    if v < 0x40 and argc is None:
                        argc = v
                elif buf[k] == 0x68:
                    imm = struct.unpack_from('<I', buf, k + 1)[0]
                    s = cstr(imm)
                    if s and len(s) >= 3 and s.isascii():
                        name_va = imm
                    elif imm < 0x40 and argc is None:
                        argc = imm
            if name_va is not None:
                entries.append({'name': cstr(name_va), 'argc': argc,
                                'handler': '0x%06x' % handler,
                                'name_va': '0x%06x' % name_va,
                                'registration_pc': '0x%06x' % (i - 3 + IMAGE_BASE)})
            i += 7
        else:
            i += 1

    by_name = {}
    for e in entries:
        by_name.setdefault(e['name'], e)

    cmd_table = json.load(io.open(CMD_TABLE, encoding='utf-8'))
    joined = []
    missing = []
    for c in cmd_table:
        e = by_name.get(c['name'])
        if e is None:
            missing.append({'id': c['id'], 'name': c['name']})
            continue
        joined.append({'id': c['id'], 'name': c['name'], 'argc': c['argc'],
                       'handler': e['handler'], 'name_va': e['name_va'],
                       'registration_pc': e['registration_pc']})

    result = {
        'note': '命令 id → handler 地址（A 类）：原版在 0x43E000..0x440E00 逐条注册命令'
                '（mov $handler,%ecx + push argc + push 名字 + call 0x4387d0）；'
                '名字是字典序，id 由 HCB 侧 ref/cmd_table.json 反查。'
                '由 scripts/extract-exe-cmd-dispatch.py 生成',
        'registrar': '0x004387d0（register(entry, name)）',
        'scan_range': ['0x43e000', '0x440e00'],
        'orders': len(entries),
        'joined': len(joined),
        'missing': missing,
        'commands': joined,
    }
    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    print('注册条目 %d，与 HCB 命令表连接上 %d / %d（缺 %d）'
          % (len(entries), len(joined), len(cmd_table), len(missing)))
    for want in (78, 81, 89, 90, 110, 141):
        hit = [j for j in joined if j['id'] == want]
        print('  cmd %-3d -> %s' % (want, (hit[0]['name'] + ' ' + hit[0]['handler'])
                                    if hit else 'NOT FOUND'))
    if missing:
        print('  缺:', ', '.join('%d:%s' % (m['id'], m['name'])
                                 for m in missing[:10]))
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        if io.open(OUT_JSON, encoding='utf-8').read() != text:
            print('FAIL: %s 与提取结果不一致' % OUT_JSON)
            return 1
        print('OK: %s 与提取结果逐字节一致' % OUT_JSON)
        return 0
    io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n').write(text)
    print('written %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())