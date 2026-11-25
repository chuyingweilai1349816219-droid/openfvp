#!/usr/bin/env python3
"""从原版 exe 静态提取「文字/GDI 绘制协议」，不需要运行原版游戏。

为什么要有这个脚本：docs/original-verification.md 里有一类缺口叫"要跑原版才能
对照"。但对**绘制协议**（用哪个 API、传什么参数、什么颜色、什么字号的换算公式）
来说这是不必要的——这些是 exe 里的常量与调用约定，静态静态分析就能读出来，
而且比截图对照更准确（截图只能反推，静态分析能读到原值）。

做法：
  1. 解析 PE 头 → 节表，建立 虚拟地址(VA) → 文件偏移 的映射；
  2. 解析 objdump 静态分析文本（work/iroseka_HD.asm），把 `call *0x45dXXX` 里的
     IAT 槽位解析成导入函数名（work/vm_imports.json）；
  3. 对关注的 GDI 函数，从调用点往回收 `push` 序列，还原出实参（立即数/寄存器）；
  4. 顺带把立即数形式的静态数据（如 GetGlyphOutline 的 MAT2 矩阵）从 exe 里读出来。

输出：ref/exe-gdi-params.json（可提交；里面只有参数，不含游戏数据）。

用法：
    python scripts/extract-exe-gdi.py                # 打印 + 写 ref/exe-gdi-params.json
    python scripts/extract-exe-gdi.py --check        # 只比对，不写文件（门禁用）
    python scripts/extract-exe-gdi.py --exe PATH --asm PATH
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = r"C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\iroseka_HD.exe"
DEFAULT_ASM = os.path.join(ROOT, "work", "iroseka_HD.asm")
DEFAULT_IMPORTS = os.path.join(ROOT, "work", "vm_imports.json")
OUT_JSON = os.path.join(ROOT, "ref", "exe-gdi-params.json")

# 关心的 GDI/USER 导入函数 → (函数名, 参数个数)
TARGET_FUNCS = {
    "CreateFontA": 14,
    "GetGlyphOutlineA": 7,
    "GetTextMetricsA": 2,
    "GetOutlineTextMetricsA": 3,
    "SetTextColor": 2,
    "SetBkMode": 2,
    "SelectObject": 2,
    "CreateDIBSection": 6,
    "CreateCompatibleDC": 1,
    "EnumFontFamiliesExA": 5,
    "TextOutA": 5,
    "DeleteObject": 1,
}

LINE_RE = re.compile(r"^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$")
CALL_IAT_RE = re.compile(r"call\s+\*0x([0-9a-f]+)$")
PUSH_RE = re.compile(r"^push\s+(.+)$")
IMM_RE = re.compile(r"^\$(0x[0-9a-f]+|-?\d+)$")
REG_RE = re.compile(r"^%(\w+)$")


def parse_pe(path):
    """返回 (sha256, 节表 [{'name','va','vsize','off','raw_size'}], file bytes)。"""
    with open(path, "rb") as fp:
        data = fp.read()
    if data[:2] != b"MZ":
        raise SystemExit("not a PE file: %s" % path)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe : pe + 4] != b"PE\0\0":
        raise SystemExit("bad PE signature")
    nsec, = struct.unpack_from("<H", data, pe + 6)
    opt_size, = struct.unpack_from("<H", data, pe + 20)
    opt = pe + 24
    image_base, = struct.unpack_from("<I", data, opt + 28)
    sec = opt + opt_size
    sections = []
    for i in range(nsec):
        o = sec + i * 40
        name = data[o : o + 8].rstrip(b"\0").decode("latin-1")
        vsize, va, raw_size, raw_off = struct.unpack_from("<IIII", data, o + 8)
        sections.append(
            {
                "name": name,
                "va": va + image_base,
                "vsize": vsize,
                "off": raw_off,
                "raw_size": raw_size,
            }
        )
    return hashlib.sha256(data).hexdigest(), sections, data


def read_va(sections, data, va, n):
    for s in sections:
        if s["va"] <= va < s["va"] + max(s["vsize"], s["raw_size"]):
            off = s["off"] + (va - s["va"])
            if off + n > len(data):
                return None
            return data[off : off + n]
    return None


def load_imports(path):
    """{'45d014': 'GDI32.dll!CreateFontA'} → {'45d014': 'CreateFontA'}"""
    with open(path, encoding="utf-8") as fp:
        raw = json.load(fp)
    return {k.lower(): v.split("!")[-1] for k, v in raw.items()}


def load_asm(path):
    ins = []
    with open(path, encoding="utf-8", errors="replace") as fp:
        for line in fp:
            m = LINE_RE.match(line.rstrip("\n"))
            if m:
                ins.append((int(m.group(1), 16), m.group(3).strip()))
    return ins


def parse_operand(tok):
    tok = tok.strip()
    m = IMM_RE.match(tok)
    if m:
        return {"kind": "imm", "value": int(m.group(1), 0)}
    m = REG_RE.match(tok)
    if m:
        return {"kind": "reg", "value": m.group(1)}
    return {"kind": "other", "value": tok}


def collect_args(ins, idx, nargs):
    """从 callsite 往前收 push，返回 [arg1..argN]（arg1 是最后一个 push）。"""
    pushes = []
    j = idx - 1
    while j >= 0 and len(pushes) < nargs:
        text = ins[j][1]
        if text.startswith("push"):
            m = PUSH_RE.match(text)
            if m:
                pushes.append(parse_operand(m.group(1)))
            else:
                pushes.append({"kind": "other", "value": text})
        elif text.startswith("call") or text.startswith("jmp") or text.startswith("ret"):
            return None
        j -= 1
    if len(pushes) < nargs:
        return None
    # 倒着收 push 得到的就是 arg1..argN（最后一个 push 是 arg1，最先被弹出）
    return pushes


def func_start(ins, idx):
    """往上找函数起点：跳过 int3 填充后的第一条指令。"""
    j = idx
    while j > 0:
        if ins[j - 1][1].startswith("ret") or ins[j - 1][1].startswith("int3"):
            if not ins[j][1].startswith("int3"):
                return ins[j][0]
        j -= 1
    return ins[0][0]


def hexva(v):
    return "0x%08x" % v


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.environ.get("FVP_EXE", DEFAULT_EXE))
    ap.add_argument("--asm", default=DEFAULT_ASM)
    ap.add_argument("--imports", default=DEFAULT_IMPORTS)
    ap.add_argument("--check", action="store_true")
    opt = ap.parse_args(argv)

    if not os.path.isfile(opt.exe):
        print("SKIP: original exe not found: %s" % opt.exe)
        return 0
    if not os.path.isfile(opt.asm):
        print("SKIP: static listing not found: %s" % opt.asm)
        return 0

    sha, sections, data = parse_pe(opt.exe)
    imports = load_imports(opt.imports)
    ins = load_asm(opt.asm)
    print("exe sha256 %s  (%d bytes, %d sections)" % (sha, len(data), len(sections)))
    print("static listing: %d instructions" % len(ins))

    result = {
        "note": (
            "从原版 exe 静态提取的绘制协议（不含游戏数据）。"
            "由 scripts/extract-exe-gdi.py 生成；tests/verify_exe_gdi_params.py 回归。"
        ),
        "exe_sha256": sha,
        "exe_size": len(data),
        "calls": {},
        "static_data": {},
    }

    # 1) 逐个关注函数收集调用点
    sites = {}
    for idx, (addr, text) in enumerate(ins):
        m = CALL_IAT_RE.match(text)
        if not m:
            continue
        slot = m.group(1).lower()
        name = imports.get(slot) or imports.get(slot.zfill(8))
        if name not in TARGET_FUNCS:
            continue
        nargs = TARGET_FUNCS[name]
        a = collect_args(ins, idx, nargs)
        sites.setdefault(name, []).append(
            {
                "site": hexva(addr),
                "func": hexva(func_start(ins, idx)),
                "args": a,
            }
        )

    for name in sorted(sites):
        result["calls"][name] = sites[name]
        print("\n%s  (%d 个调用点)" % (name, len(sites[name])))
        for s in sites[name]:
            pretty = []
            for a in s["args"] or []:
                if a is None:
                    pretty.append("?")
                elif a["kind"] == "imm":
                    pretty.append("0x%x" % a["value"])
                elif a["kind"] == "reg":
                    pretty.append("%" + a["value"])
                else:
                    pretty.append(a["value"])
            print("  %s  in %s  args=[%s]" % (s["site"], s["func"], ", ".join(pretty)))

    # 2) CreateFontA 的字号换算：把计算 height 的几条指令也记下来，便于文档引用
    cf = sites.get("CreateFontA") or []
    if cf:
        site_addr = int(cf[0]["site"], 16)
        idxs = [i for i, (a, _) in enumerate(ins) if a < site_addr][-24:]
        result["calls"]["CreateFontA"][0]["height_window"] = [
            "%s: %s" % (hexva(ins[i][0]), ins[i][1]) for i in idxs
        ]

    # 3) 静态数据：GetGlyphOutline 的 MAT2 指针（第 7 个参数）
    mat2_ptrs = set()
    for s in sites.get("GetGlyphOutlineA") or []:
        if s["args"] and len(s["args"]) > 6 and s["args"][6]["kind"] == "imm":
            mat2_ptrs.add(s["args"][6]["value"])
    for ptr in sorted(mat2_ptrs):
        raw = read_va(sections, data, ptr, 16)
        if raw is None:
            continue
        vals = struct.unpack("<4i", raw)
        result["static_data"][hexva(ptr)] = {
            "kind": "MAT2",
            "raw": raw.hex(),
            "fixed16_16": ["%.4f" % (v / 65536.0) for v in vals],
        }
        print("\nMAT2 @ %s = %s (raw %s)" % (hexva(ptr), result["static_data"][hexva(ptr)]["fixed16_16"], raw.hex()))

    # 4) 不变量（写进 JSON，门禁直接读，避免门禁里再算一遍）
    invariants = {}
    if cf:
        cfargs = cf[0]["args"] or []
        result["calls"]["CreateFontA"][0]["named"] = {
            "nHeight": "computed",
            "nWidth": cfargs[1] if len(cfargs) > 1 else None,
            "nEscapement": cfargs[2] if len(cfargs) > 2 else None,
            "nOrientation": cfargs[3] if len(cfargs) > 3 else None,
            "fnWeight": cfargs[4] if len(cfargs) > 4 else None,
            "fdwItalic": cfargs[5] if len(cfargs) > 5 else None,
            "fdwUnderline": cfargs[6] if len(cfargs) > 6 else None,
            "fdwStrikeOut": cfargs[7] if len(cfargs) > 7 else None,
            "fdwCharSet": cfargs[8] if len(cfargs) > 8 else None,
            "fdwOutputPrecision": cfargs[9] if len(cfargs) > 9 else None,
            "fdwClipPrecision": cfargs[10] if len(cfargs) > 10 else None,
            "fdwQuality": cfargs[11] if len(cfargs) > 11 else None,
            "fdwPitchAndFamily": cfargs[12] if len(cfargs) > 12 else None,
            "lpszFace": cfargs[13] if len(cfargs) > 13 else None,
        }
        cs = cfargs[8] if len(cfargs) > 8 else None
        invariants["charset_is_shiftjis"] = bool(cs and cs.get("kind") == "imm" and cs["value"] == 0x80)
        invariants["charset_value"] = cs.get("value") if cs and cs.get("kind") == "imm" else None

    gg = sites.get("GetGlyphOutlineA") or []
    fmts = []
    for s in gg:
        a = s["args"]
        if a and len(a) > 2 and a[2] and a[2]["kind"] == "imm":
            fmts.append(a[2]["value"])
    invariants["glyph_uformats"] = sorted(set(fmts))
    invariants["glyph_sites"] = len(gg)
    invariants["mat2_all_same"] = len(mat2_ptrs) == 1
    result["invariants"] = invariants

    print("\n不变量: %s" % json.dumps(invariants, ensure_ascii=False, sort_keys=True))

    text = json.dumps(result, ensure_ascii=False, indent=1) + "\n"
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print("FAIL: %s 不存在" % OUT_JSON)
            return 1
        old = open(OUT_JSON, encoding="utf-8").read()
        if old != text:
            print("FAIL: 提取结果与 ref/exe-gdi-params.json 不一致")
            import difflib

            for line in list(difflib.unified_diff(old.splitlines(), text.splitlines(), "ref", "new", lineterm=""))[:40]:
                print(line)
            return 1
        print("OK: 与 ref/exe-gdi-params.json 一致")
        return 0

    with open(OUT_JSON, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(text)
    print("\n写入 %s" % OUT_JSON)
    return 0


if __name__ == "__main__":
    sys.exit(main())