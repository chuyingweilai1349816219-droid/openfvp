#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gfx_dump.py -- dump one fully-specified frame of the FVP render model.

The C renderer (src/gfx.c) never re-runs the script VM: this tool runs the
Python reference (work/render.py) to the checkpoint, snapshots the complete
render state (prim table / graph slots / parts overlays / text windows) and
writes it as UTF-8 JSON.  test_gfx reads that JSON and reproduces the frame.

Outputs (default fvp/out/c):
    gfx-scene.json          the frame state + the reference draw list
    gfx-font-<size>.bin     bitmap font atlas for the sizes used by the text
    gfx-frame-py.png        the Python composited frame (sanity: identical to
                            fvp/out/render/story_00528000.png)

The JSON schema (version 1) is documented in fvp/docs/c-gfx.md.

Usage:
    python fvp/tests/gfx_dump.py                     # 528000, story frame
    python fvp/tests/gfx_dump.py --at 534000
    python fvp/tests/gfx_dump.py --out fvp/out/c --no-png
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)          # fvp/
WORK = os.path.join(ROOT, 'work')
GAME = r'C:\\Users\\Mechrevo\\Downloads\\' + '\u4e94\u5f69\u6591\u6593\u7684\u4e16\u754cHD'
DEFAULT_OUT = os.path.join(ROOT, 'out', 'c')
REF_PNG = os.path.join(ROOT, 'out', 'render', 'story_00528000.png')

# render.py hides fvp/work from sys.path while importing numpy/PIL, so insert
# the work directory the same way run_render.py does.
for _p in list(sys.path):
    if os.path.abspath(_p or '.') == WORK:
        sys.path.remove(_p)
sys.path.insert(0, WORK)

FONT_CANDIDATES = [
    r'C:\Windows\Fonts\msgothic.ttc',
    r'C:\Windows\Fonts\YuGothM.ttc',
    r'C:\Windows\Fonts\meiryo.ttc',
    r'C:\Windows\Fonts\msyh.ttc',
    r'C:\Windows\Fonts\simhei.ttf',
]

# ---------------------------------------------------------------------------
# bitmap font atlas (format documented in docs/c-gfx.md section "bitmap font")
#
#   0x00  4  'GFXF'
#   0x04  u32 version (1)
#   0x08  u32 size       em size in pixels (matches PIL font size)
#   0x0C  u32 margin     padding around the ascender box (for stroke)
#   0x10  u32 ascent
#   0x14  u32 descent
#   0x18  u32 count
#   0x1C  count * { u32 codepoint, u32 advance, u32 w }
#         pixel blocks: glyph i has h = ascent+descent+2*margin rows and
#         w columns, 8-bit coverage; three masks per glyph (glyph,
#         stroke width 1, stroke width 2), stored back to back in index
#         order.
# ---------------------------------------------------------------------------
GFXF_MAGIC = b'GFXF'
GFXF_VERSION = 2
GFXF_HEADER = 28
GFXF_RECORD = 12


def _font_path():
    for p in FONT_CANDIDATES:
        if os.path.exists(p):
            return p
    raise SystemExit('gfx_dump: no TrueType font found (tried msgothic/meiryo/msyh)')


def _truetype(size):
    from PIL import ImageFont
    return ImageFont.truetype(_font_path(), size)


def write_font(path, size, chars):
    """Rasterise `chars` at `size` the same way PIL's ImageDraw.text does and
    write a GFXF v2 atlas.  Per glyph three coverage masks are stored:

        layer 0: glyph   -- text(fill=255)
        layer 1: stroke  -- text(fill=255, stroke_width=1, stroke_fill=255)
        layer 2: stroke  -- text(fill=255, stroke_width=2, stroke_fill=255)

    The stroke layers are the dilated masks PIL/FreeType produce, so gfx.c can
    paint text with the exact same coverage the reference PNG used instead of
    approximating the outline with a box dilation.

    Returns the font metrics dict.
    """
    from PIL import Image, ImageDraw

    font = _truetype(size)
    ascent, descent = font.getmetrics()
    margin = 4
    h = ascent + descent + 2 * margin
    layers = (0, 1, 2)
    glyphs = []
    for ch in sorted(set(chars)):
        adv = int(round(font.getlength(ch)))
        if adv <= 0:
            adv = 1
        w = adv + 2 * margin
        blocks = []
        for sw in layers:
            im = Image.new('L', (w, h), 0)
            draw = ImageDraw.Draw(im)
            if sw == 0:
                draw.text((margin, margin), ch, font=font, fill=255,
                          anchor='la')
            else:
                draw.text((margin, margin), ch, font=font, fill=255,
                          anchor='la', stroke_width=sw, stroke_fill=255)
            blocks.append(im.tobytes())
        glyphs.append((ord(ch), adv, w, blocks))
    hdr = struct.pack('<4sIIIIII', GFXF_MAGIC, GFXF_VERSION, size, margin,
                      ascent, descent, len(glyphs))
    recs = b''.join(struct.pack('<III', cp, adv, w)
                    for (cp, adv, w, _blocks) in glyphs)
    pix = b''.join(b''.join(blocks) for (_cp, _adv, _w, blocks) in glyphs)
    with open(path, 'wb') as f:
        f.write(hdr)
        f.write(recs)
        f.write(pix)
    return dict(path=os.path.basename(path), size=size, ascent=ascent,
                descent=descent, margin=margin, glyphs=len(glyphs),
                cell_h=h, layers=len(layers),
                bytes=GFXF_HEADER + GFXF_RECORD * len(glyphs) + len(pix))


# ---------------------------------------------------------------------------
# scene snapshot
# ---------------------------------------------------------------------------
def text_font_size(vm, idx):
    """Mirror of render.RenderVM._draw_text's font-size choice."""
    lay = vm.TEXT_LAYOUT.get(idx, dict(size=30))
    size = lay['size']
    if idx == 0:
        w = vm.texts.get(0)
        h = w.size[1] if w is not None else 32
        size = max(18, min(40, h if h > 8 else 34))
    return int(size)


def overlay_records(vm, slot):
    """Resolve Graph.overlays [(x, y, PIL img)] back to (part id, frame, name)."""
    g = vm.graphs.get(slot)
    out = []
    if g is None:
        return out
    for (ox, oy, img) in g.overlays:
        rec = dict(x=int(ox), y=int(oy), name=None, frame=-1, parts=-1)
        for pid, part in vm.parts.items():
            if part.graph_slot != slot:
                continue
            try:
                imgs = part.images()
            except Exception:
                continue
            for fi, pim in enumerate(imgs):
                if pim is img:
                    rec.update(name=part.name, frame=fi, parts=int(pid))
                    break
            if rec['name'] is not None:
                break
        if rec['name'] is None:
            rec['warn'] = 'overlay-unresolved'
        out.append(rec)
    return out


def prim_record(p):
    return dict(id=int(p.id), type=int(p.type), draw=int(p.draw),
                alpha=int(p.alpha), blend=int(p.blend), z=int(p.z),
                x=int(p.x), y=int(p.y), w=int(p.w), h=int(p.h),
                u=int(p.u), v=int(p.v), op1=int(p.op1), op2=int(p.op2),
                angle=int(p.angle), sx=int(p.scale_x), sy=int(p.scale_y),
                graph=int(p.graph), tw=int(p.tw), tile=int(p.tile),
                flags=int(p.flags), parent=int(p.parent),
                children=[int(c) for c in p.children])


def scene_json(vm, info, step):
    graphs = []
    for slot in sorted(vm.graphs):
        g = vm.graphs[slot]
        graphs.append(dict(slot=int(slot), name=g.name, w=int(g.w), h=int(g.h),
                           x=int(g.x), y=int(g.y), frames=int(g.frames),
                           frame=int(g.frame_sel),
                           overlays=overlay_records(vm, slot)))
    texts = []
    for idx in sorted(vm.texts):
        w = vm.texts[idx]
        vis = vm._visible_text(idx)
        texts.append(dict(idx=int(idx), lines=list(vis),
                          stored=list(w.lines),
                          color=[int(c) & 255 for c in w.color],
                          size=[int(w.size[0]), int(w.size[1])],
                          pos=(None if w.pos is None
                               else [int(w.pos[0]), int(w.pos[1])]),
                          out_size=[int(w.out_size[0]), int(w.out_size[1])],
                          last_text=w.last_text))
    prims = [prim_record(vm.prims[pid]) for pid in sorted(vm.prims)]
    return dict(
        version=1,
        step=int(step),
        game=GAME,
        canvas=[int(vm.CANVAS_W) if hasattr(vm, 'CANVAS_W') else 1920,
                int(vm.CANVAS_H) if hasattr(vm, 'CANVAS_H') else 1080],
        background=[0, 0, 0, 255],
        prims=prims,
        graphs=graphs,
        texts=texts,
        expect_draw=[dict(prim=int(i[0]), name=i[1], x=int(i[2]), y=int(i[3]),
                          w=int(i[4]), h=int(i[5]), mode=i[6], z=int(i[7]),
                          alpha=int(i[8])) for i in info],
        text_layout=dict((str(k), dict(x=int(v['x']), y=int(v['y']),
                                       maxw=int(v['maxw']),
                                       size=int(v['size']),
                                       lines=int(v['lines'])))
                         for k, v in vm.TEXT_LAYOUT.items()),
    )


def build_vm(script, enc, hold_text):
    import render
    cmds, names = render.load_cmd_table()
    vm = render.RenderVM(script, enc=enc, cmds=cmds, names=names, quiet=True)
    vm.click_period = 4000
    vm.enable_threads = False
    vm.hold_text = bool(hold_text)
    vm.hold_window = hold_text
    vm.start()
    return vm


def main(argv=None):
    ap = argparse.ArgumentParser(description='dump one FVP render frame as JSON')
    ap.add_argument('--at', type=int, default=528000, help='VM step checkpoint')
    ap.add_argument('--script', default='jp', help="'jp', 'zh' or a .hcb path")
    ap.add_argument('--enc', default=None)
    ap.add_argument('--hold-text', type=int, default=30000,
                    help='same snapshot trick as run_render.py --hold-text')
    ap.add_argument('--game', default=None,
                    help='game directory (default: render_assets.GAME)')
    ap.add_argument('--ref', default=None,
                    help='reference PNG used by the snapshot check')
    ap.add_argument('--out', default=DEFAULT_OUT)
    ap.add_argument('--no-png', action='store_true')
    ap.add_argument('--no-font', action='store_true')
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args(argv)

    global GAME, REF_PNG
    if a.game:
        GAME = a.game
        import render as _render
        import render_assets as _ra
        _ra.GAME = GAME
        _render.GAME = GAME
    if a.ref:
        REF_PNG = a.ref
    else:
        _cand = os.path.join(ROOT, 'out', 'render', 'story_%08d.png' % a.at)
        if os.path.exists(_cand):
            REF_PNG = _cand
    script, enc = a.script, a.enc
    if script == 'jp':
        script = os.path.join(GAME, 'iroseka.hcb')
        enc = enc or 'cp932'
    elif script == 'zh':
        script = os.path.join(GAME, '.iroseka.hcb')
        enc = enc or 'gbk'
    enc = enc or 'cp932'

    os.makedirs(a.out, exist_ok=True)
    vm = build_vm(script, enc, a.hold_text)
    if a.at > vm.steps:
        vm.run(a.at)
    if vm.steps < a.at:
        print('gfx_dump: WARNING ran out of script at step %d (want %d)'
              % (vm.steps, a.at), file=sys.stderr)

    canvas, info = vm.render_frame()
    body = scene_json(vm, info, a.at)

    # ---- bitmap fonts for every text window that will be drawn -----------
    fonts = []
    if not a.no_font:
        sizes = {}
        for t in body['texts']:
            if not t['lines']:
                continue
            size = text_font_size(vm, t['idx'])
            sizes.setdefault(size, set()).update(''.join(t['lines']))
        for size in sorted(sizes):
            name = 'gfx-font-%d.bin' % size
            meta = write_font(os.path.join(a.out, name), size, sizes[size])
            fonts.append(meta)
            if not a.quiet:
                print('[font] %-18s size=%3d glyphs=%3d %6d bytes'
                      % (name, size, meta['glyphs'], meta['bytes']))
    body['fonts'] = fonts

    path = os.path.join(a.out, 'gfx-scene.json')
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(body, f, ensure_ascii=False, indent=1)
    if not a.quiet:
        print('[scene] step=%d prims=%d graphs=%d texts=%d draw=%d -> %s'
              % (body['step'], len(body['prims']), len(body['graphs']),
                 len(body['texts']), len(body['expect_draw']), path))
        for d in body['expect_draw']:
            print('  draw %4d %-38s %5d %5d %4dx%-4d %s a=%d'
                  % (d['prim'], d['name'], d['x'], d['y'], d['w'], d['h'],
                     d['mode'], d['alpha']))

    # ---- Python-side composited frame (fidelity check of the snapshot) ---
    if not a.no_png:
        py_path = os.path.join(a.out, 'gfx-frame-py.png')
        canvas.save(py_path)
        if os.path.exists(REF_PNG):
            _compare_png(py_path, REF_PNG, a.quiet)
    return 0


def _compare_png(py_path, ref_path, quiet):
    """The dump must describe exactly the state of the published reference."""
    try:
        from PIL import Image, ImageChops
        import numpy as np
    except Exception:
        return
    a = Image.open(py_path).convert('RGBA')
    b = Image.open(ref_path).convert('RGBA')
    if a.size != b.size:
        print('[check] SIZE MISMATCH %s vs %s' % (a.size, b.size))
        return
    da = np.asarray(a, dtype=np.int16)
    db = np.asarray(b, dtype=np.int16)
    diff = np.abs(da - db)
    n = int((diff.max(axis=2) > 0).sum())
    if not quiet:
        print('[check] python re-render vs %s: %d differing px, max channel '
              'diff %d' % (os.path.basename(ref_path), n, int(diff.max())))
    if n:
        print('[check] WARNING python re-render differs from the reference '
              'PNG; the dump may not match the published frame', file=sys.stderr)


if __name__ == '__main__':
    sys.exit(main())
