# FVP 引擎（C 实现）
#
#   make             构建 bin/fvm.exe（脚本 VM，已通过逐字节等价验证）
#   make lib        构建 bin/libfvp.a（含音频层，给 Android / 桌面壳链接）
#   make test       构建测试程序（音频解码自检）
#   make clean
#
# 生成的头文件在 src/gen_*.h，由
#   python tests/gen_tables.py <script.hcb>
# 产出并已入库，构建只需要 C99 编译器。

CC      ?= gcc
AR      ?= ar
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -fwrapv
LDFLAGS ?=
EXE     ?= .exe

CORE_SRC = src/fvp.c src/encoding.c src/hcb.c src/vm.c src/syscall.c src/main.c
SRC      = src/fvp.c src/encoding.c src/hcb.c src/vm.c src/syscall.c
SND_SRC  = src/snd.c
RES_SRC  = src/bin.c src/inflate.c src/hzc1.c
SAVE_OBJ = src/save.o
SAVEVM_OBJ = src/save_vm.o
FLAGS_OBJ  = src/flags.o
TEXTATTR_OBJ = src/textattr.o
THUMB_OBJ = src/thumb.o
TIMER_OBJ = src/timer.o
HISTORY_OBJ = src/history.o

OBJ      = $(CORE_SRC:.c=.o) $(SAVE_OBJ) $(SAVEVM_OBJ) $(FLAGS_OBJ) \
           $(TEXTATTR_OBJ) $(THUMB_OBJ) $(TIMER_OBJ) $(HISTORY_OBJ)
LIB_OBJ  = $(SRC:.c=.o) $(SND_SRC:.c=.o) $(RES_SRC:.c=.o) $(SAVE_OBJ) \
           $(SAVEVM_OBJ) $(FLAGS_OBJ) $(TEXTATTR_OBJ) $(THUMB_OBJ) $(TIMER_OBJ) $(HISTORY_OBJ)

BIN = bin/fvm$(EXE)
LIB = bin/libfvp.a

HEADERS = src/fvp.h src/encoding.h src/hcb.h src/vm.h src/syscall.h \
          src/snd.h src/bin.h src/inflate.h src/hzc1.h \
          src/textattr.h src/timer.h src/history.h src/motion.h src/save.h \
          src/save_vm.h \
          src/flags.h src/thumb.h \
          src/gen_cp932.h src/gen_gbk.h src/gen_cmd_table.h

all: $(BIN)

$(BIN): $(OBJ) | bin
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

lib: $(LIB)

$(LIB): $(LIB_OBJ) | bin
	$(AR) rcs $@ $(LIB_OBJ)

test: bin/test_snd$(EXE)

bin/test_snd$(EXE): tests/test_snd.c src/snd.c src/snd.h | bin
	$(CC) $(CFLAGS) -o $@ tests/test_snd.c src/snd.c $(LDFLAGS) -lm

src/%.o: src/%.c $(HEADERS) | src
	$(CC) $(CFLAGS) -c -o $@ $<

# 音频层单独一条规则：它依赖 third_party/stb_vorbis.c（实现文件被 include）
src/snd.o: src/snd.c src/snd.h src/third_party/stb_vorbis.c $(HEADERS) | src
	$(CC) $(CFLAGS) -c -o $@ $<

bin:
	-mkdir bin

src:
	-mkdir src

clean:
	-rm -f $(OBJ) $(LIB_OBJ) $(BIN) $(LIB) bin/test_snd$(EXE)

.PHONY: all lib test clean

# ---------------------------------------------------------------------------
# Win32 桌面壳（用法与限制见 docs/win32-shell.md）
#
#   make win32                    构建 bin\fvp.exe
#   make win32 GFX_FORCE_STUB=1  强制链接 tests/stub_gfx.c 的测试图案
#
# 只追加本节，不改变 all / lib / test 的语义。src/gfx.c 一旦落地就自动编进
# 壳里；在那之前用 tests/stub_gfx.c，保证窗口/帧循环/输入/音频可以先验证。
# ---------------------------------------------------------------------------
WIN32_DIR   = src/platform/win32
WIN32_SHELL = $(WIN32_DIR)/main_win32.c $(WIN32_DIR)/win32_window.c \
               $(WIN32_DIR)/win32_audio.c $(WIN32_DIR)/win32_log.c
WIN32_CORE  = src/fvp.c src/encoding.c src/hcb.c src/vm.c src/syscall.c src/snd.c \
               src/save.c src/save_vm.c src/flags.c src/textattr.c src/thumb.c \
               src/timer.c src/history.c
WIN32_RES   = src/bin.c src/hzc1.c src/inflate.c
WIN32_GFX_H = $(wildcard src/gfx.h)

ifneq ($(GFX_FORCE_STUB),1)
ifneq ($(wildcard src/gfx.c),)
WIN32_GFX_C   = src/gfx.c src/motion.c
WIN32_GFX_DEF =
else
WIN32_GFX_C   = tests/stub_gfx.c
WIN32_GFX_DEF = -DFVP_STUB_GFX
endif
else
WIN32_GFX_C   = tests/stub_gfx.c
WIN32_GFX_DEF = -DFVP_STUB_GFX
endif

WIN32_SRC = $(WIN32_CORE) $(WIN32_RES) $(WIN32_GFX_C) $(WIN32_SHELL)
WIN32_BIN = bin/fvp$(EXE)

win32: $(WIN32_BIN)

$(WIN32_BIN): $(WIN32_SRC) $(HEADERS) $(WIN32_GFX_H) | bin
	$(CC) $(CFLAGS) $(WIN32_GFX_DEF) -Isrc -I$(WIN32_DIR) -o $@ $(WIN32_SRC) \
	    -mwindows -lgdi32 -luser32 -lwinmm -lshell32 -lole32 -lm $(LDFLAGS)

.PHONY: win32

# ---------------------------------------------------------------------------
# gfx 渲染层（追加节，不改 all / lib / test 的语义；详见 docs/c-gfx.md）
#
#   make gfx        构建 bin/libfvp_gfx.a（gfx.c + bin.c + hzc1.c + inflate.c）
#   make test_gfx   构建 bin/test_gfx.exe
#
#   bin/test_gfx.exe fvp/out/c/gfx-scene.json -o fvp/out/c
# ---------------------------------------------------------------------------
GFX_OBJ  = src/gfx.o src/bin.o src/hzc1.o src/inflate.o src/motion.o
GFX_LIB  = bin/libfvp_gfx.a
GFX_TEST = bin/test_gfx$(EXE)

gfx: $(GFX_LIB)

$(GFX_LIB): $(GFX_OBJ) | bin
	$(AR) rcs $@ $(GFX_OBJ)

test_gfx: $(GFX_TEST)

$(GFX_TEST): tests/test_gfx.c src/gfx.h $(GFX_OBJ) | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_gfx.c $(GFX_OBJ) $(LDFLAGS) -lm

.PHONY: gfx test_gfx test_gfx_motion

clean-gfx:
	-rm -f $(GFX_OBJ) $(GFX_LIB) $(GFX_TEST)

.PHONY: clean-gfx

# ---------------------------------------------------------------------------
# 存档层（追加节，不改 all / lib / test 的语义；详见 docs/save-format.md §7）
#   make test_save     构建 bin/test_save.exe
#   bin/test_save.exe                                   自检（合成数据）
#   bin/test_save.exe --savebin <dir>/save.bin <dir>/save/sNNN.bin ...
#                                                         真实样本逐字节回环
# ---------------------------------------------------------------------------
SAVE_TEST = bin/test_save$(EXE)

test_save: $(SAVE_TEST)

$(SAVE_TEST): tests/test_save.c src/save.c src/save.h | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_save.c src/save.c $(LDFLAGS)

$(SAVE_OBJ): src/save.c src/save.h | src
	$(CC) $(CFLAGS) -c -o $@ $<

# save.bin → VM 全局变量桥（依 fvp.h 的 FvpValue，故不与 save.c 合并）
$(SAVEVM_OBJ): src/save_vm.c src/save_vm.h src/save.h src/fvp.h | src
	$(CC) $(CFLAGS) -c -o $@ $<

SAVEVM_TEST = bin/test_save_vm$(EXE)

test_save_vm: $(SAVEVM_TEST)

$(SAVEVM_TEST): tests/test_save_vm.c src/save_vm.c src/save.c src/save_vm.h \
        src/save.h | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_save_vm.c src/save_vm.c src/save.c $(LDFLAGS)

.PHONY: test_save test_save_vm test_flags test_textattr

# ---------------------------------------------------------------------------
# 全局 flag 位图（FlagGet/FlagSet 的存储层 + save.bin 播种，见 docs/vm-commands.md）
#   make test_flags
#   bin/test_flags.exe [<save.bin> ...]
# ---------------------------------------------------------------------------
FLAGS_TEST = bin/test_flags$(EXE)

test_flags: $(FLAGS_TEST)

$(FLAGS_TEST): tests/test_flags.c src/flags.c src/flags.h src/save.c \
        src/save.h src/save_vm.c src/save_vm.h src/fvp.h | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_flags.c src/flags.c src/save.c \
	    src/save_vm.c $(LDFLAGS)

$(FLAGS_OBJ): src/flags.c src/flags.h | src
	$(CC) $(CFLAGS) -c -o $@ $<

clean-save:
	-rm -f $(SAVE_OBJ) $(SAVEVM_OBJ) $(FLAGS_OBJ) $(SAVE_TEST) $(SAVEVM_TEST) \
	    $(FLAGS_TEST) bin/libfvp_save.a

.PHONY: clean-save

# ---------------------------------------------------------------------------
# 文本窗口属性（TextColor/TextSize/TextSpeed 的存储层）
#   make test_textattr
# ---------------------------------------------------------------------------
TEXTATTR_TEST = bin/test_textattr$(EXE)

test_textattr: $(TEXTATTR_TEST)

$(TEXTATTR_TEST): tests/test_textattr.c src/textattr.c src/textattr.h | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_textattr.c src/textattr.c $(LDFLAGS)

$(TEXTATTR_OBJ): src/textattr.c src/textattr.h | src
	$(CC) $(CFLAGS) -c -o $@ $<

.PHONY: clean-textattr

clean-textattr:
	-rm -f $(TEXTATTR_OBJ) $(TEXTATTR_TEST)

# ---------------------------------------------------------------------------
# 存档缩略图缩放（1920x1080 BGRA -> 200x113，box filter）
#   make test_thumb
# ---------------------------------------------------------------------------
THUMB_TEST = bin/test_thumb$(EXE)

test_thumb: $(THUMB_TEST)

$(THUMB_TEST): tests/test_thumb.c src/thumb.c src/thumb.h | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_thumb.c src/thumb.c $(LDFLAGS)

$(THUMB_OBJ): src/thumb.c src/thumb.h | src
	$(CC) $(CFLAGS) -c -o $@ $<

clean-thumb:
	-rm -f $(THUMB_OBJ) $(THUMB_TEST)

.PHONY: clean-thumb

# ---------------------------------------------------------------------------
# 动画补间接线（gfx 侧：Motion* -> 图元字段，默认关闭）
#   make test_gfx_motion
# ---------------------------------------------------------------------------
GFX_MOTION_TEST = bin/test_gfx_motion$(EXE)

test_gfx_motion: $(GFX_MOTION_TEST)

$(GFX_MOTION_TEST): tests/test_gfx_motion.c src/gfx.h $(GFX_OBJ) | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_gfx_motion.c $(GFX_OBJ) $(LDFLAGS) -lm

clean-gfx-motion:
	-rm -f $(GFX_MOTION_TEST)

.PHONY: test_gfx_motion clean-gfx-motion

# ---------------------------------------------------------------------------
# 定时器语义（TimerSet/TimerGet/TimerSuspend）
#   make test_timer
# ---------------------------------------------------------------------------
TIMER_TEST = bin/test_timer$(EXE)

test_timer: $(TIMER_TEST)

$(TIMER_TEST): tests/test_timer.c src/timer.c src/timer.h | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_timer.c src/timer.c $(LDFLAGS)

clean-timer:
	-rm -f $(TIMER_TEST)

.PHONY: test_timer clean-timer

# ---------------------------------------------------------------------------
# 逐字显示（TextSpeed + TextSuspendChr 的渲染侧）
#   make test_typewriter
# ---------------------------------------------------------------------------
TYPEWRITER_TEST = bin/test_typewriter$(EXE)

test_typewriter: $(TYPEWRITER_TEST)

$(TYPEWRITER_TEST): tests/test_typewriter.c src/gfx.h $(GFX_OBJ) | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_typewriter.c $(GFX_OBJ) $(LDFLAGS) -lm

clean-typewriter:
	-rm -f $(TYPEWRITER_TEST)

.PHONY: test_typewriter clean-typewriter

# ---------------------------------------------------------------------------
# 纹理缓存 LRU 上限（长跑内存不再无界增长）
#   make test_tex_cache
# ---------------------------------------------------------------------------
TEX_CACHE_TEST = bin/test_tex_cache$(EXE)

test_tex_cache: $(TEX_CACHE_TEST)

$(TEX_CACHE_TEST): tests/test_tex_cache.c src/gfx.h $(GFX_OBJ) | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_tex_cache.c $(GFX_OBJ) $(LDFLAGS) -lm

clean-tex-cache:
	-rm -f $(TEX_CACHE_TEST)

.PHONY: test_tex_cache clean-tex-cache

# ---------------------------------------------------------------------------
# 台词历史记录（HistorySet 26 / HistoryGet 25 的 100 条环形缓冲）
#   make test_history
# ---------------------------------------------------------------------------
HISTORY_TEST = bin/test_history$(EXE)

test_history: $(HISTORY_TEST)

$(HISTORY_TEST): tests/test_history.c src/history.c src/history.h | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_history.c src/history.c $(LDFLAGS)

clean-history:
	-rm -f $(HISTORY_TEST)

.PHONY: test_history clean-history

# ---------------------------------------------------------------------------
# 命中测试 PrimHit(76)（gfx 层的父链累加 + 矩形判定）
#   make test_prim_hit
# ---------------------------------------------------------------------------
PRIM_HIT_TEST = bin/test_prim_hit$(EXE)

test_prim_hit: $(PRIM_HIT_TEST)

$(PRIM_HIT_TEST): tests/test_prim_hit.c src/gfx.h $(GFX_OBJ) | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_prim_hit.c $(GFX_OBJ) $(LDFLAGS) -lm

clean-prim-hit:
	-rm -f $(PRIM_HIT_TEST)

.PHONY: test_prim_hit clean-prim-hit

# ---------------------------------------------------------------------------
# 跨图集字形兜底（某套图集缺字时从别的图集缩放取，而不是画豆腐块）
#   make test_glyph_fallback
# ---------------------------------------------------------------------------
GLYPH_FALLBACK_TEST = bin/test_glyph_fallback$(EXE)

test_glyph_fallback: $(GLYPH_FALLBACK_TEST)

$(GLYPH_FALLBACK_TEST): tests/test_glyph_fallback.c src/gfx.h $(GFX_OBJ) | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_glyph_fallback.c $(GFX_OBJ) $(LDFLAGS) -lm

clean-glyph-fallback:
	-rm -f $(GLYPH_FALLBACK_TEST)

.PHONY: test_glyph_fallback clean-glyph-fallback

# ---------------------------------------------------------------------------
# 补间时间轴（Motion* 的数学核心，含 Motion*Stop 的撤销语义）
#   make test_motion
# ---------------------------------------------------------------------------
MOTION_TEST = bin/test_motion$(EXE)

test_motion: $(MOTION_TEST)

$(MOTION_TEST): tests/test_motion.c src/motion.c src/motion.h | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_motion.c src/motion.c $(LDFLAGS) -lm

clean-motion:
	-rm -f $(MOTION_TEST)

.PHONY: test_motion clean-motion

# ---------------------------------------------------------------------------
# 控制 / 光标这一组（ControlMask 8 / ControlPulse 9 / CursorChange 10 /
# CursorMove 11）的状态语义
#   make test_control
# ---------------------------------------------------------------------------
CONTROL_TEST = bin/test_control$(EXE)

test_control: $(CONTROL_TEST)

$(CONTROL_TEST): tests/test_control.c src/vm.c src/vm.h $(OBJ) | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_control.c $(filter-out src/main.o,$(OBJ)) $(LDFLAGS) -lm

clean-control:
	-rm -f $(CONTROL_TEST)

.PHONY: test_control clean-control

# ---------------------------------------------------------------------------
# GraphRGB(23) / SysProjFolder(109) 的状态语义
#   make test_graphcolor
# ---------------------------------------------------------------------------
GRAPHCOLOR_TEST = bin/test_graphcolor$(EXE)

test_graphcolor: $(GRAPHCOLOR_TEST)

$(GRAPHCOLOR_TEST): tests/test_graphcolor.c src/vm.c src/vm.h $(OBJ) | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_graphcolor.c $(filter-out src/main.o,$(OBJ)) $(LDFLAGS) -lm

clean-graphcolor:
	-rm -f $(GRAPHCOLOR_TEST)

.PHONY: test_graphcolor clean-graphcolor

# ---------------------------------------------------------------------------
# TextBuff(110) 的文本窗口缓冲尺寸状态
#   make test_textbuff
# ---------------------------------------------------------------------------
TEXTBUFF_TEST = bin/test_textbuff$(EXE)

test_textbuff: $(TEXTBUFF_TEST)

$(TEXTBUFF_TEST): tests/test_textbuff.c src/vm.c src/vm.h $(OBJ) | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_textbuff.c $(filter-out src/main.o,$(OBJ)) $(LDFLAGS) -lm

clean-textbuff:
	-rm -f $(TEXTBUFF_TEST)

.PHONY: test_textbuff clean-textbuff

# ---------------------------------------------------------------------------
# GaijiLoad(21) / V3DMotion(142) 的参数校验与记录
#   make test_gaiji
# ---------------------------------------------------------------------------
GAIJI_TEST = bin/test_gaiji$(EXE)

test_gaiji: $(GAIJI_TEST)

$(GAIJI_TEST): tests/test_gaiji.c src/vm.c src/vm.h $(OBJ) | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_gaiji.c $(filter-out src/main.o,$(OBJ)) $(LDFLAGS) -lm

clean-gaiji:
	-rm -f $(GAIJI_TEST)

.PHONY: test_gaiji clean-gaiji

# ---------------------------------------------------------------------------
# 外字渲染（GaijiLoad 21 → gfx_gaiji_add → draw_text_run 接管）
#   make test_gaiji_render
# ---------------------------------------------------------------------------
GAIJI_RENDER_TEST = bin/test_gaiji_render$(EXE)

test_gaiji_render: $(GAIJI_RENDER_TEST)

$(GAIJI_RENDER_TEST): tests/test_gaiji_render.c src/gfx.h $(GFX_OBJ) | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_gaiji_render.c $(GFX_OBJ) $(LDFLAGS) -lm

clean-gaiji-render:
	-rm -f $(GAIJI_RENDER_TEST)

.PHONY: test_gaiji_render clean-gaiji-render

# ---------------------------------------------------------------------------
# 文本图元定位（PrimSetText 86 / --text-prims）+ 图元 alpha 乘进文字
#   make test_textprims
# ---------------------------------------------------------------------------
TEXTPRIMS_TEST = bin/test_textprims$(EXE)

test_textprims: $(TEXTPRIMS_TEST)

$(TEXTPRIMS_TEST): tests/test_textprims.c src/gfx.h $(GFX_OBJ) | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_textprims.c $(GFX_OBJ) $(LDFLAGS) -lm

clean-textprims:
	-rm -f $(TEXTPRIMS_TEST)

.PHONY: test_textprims clean-textprims

# ---------------------------------------------------------------------------
# Rand(92) / TextRepaint(124)
#   make test_vmmisc
# ---------------------------------------------------------------------------
VMMISC_TEST = bin/test_vmmisc$(EXE)

test_vmmisc: $(VMMISC_TEST)

$(VMMISC_TEST): tests/test_vmmisc.c src/vm.c src/vm.h $(OBJ) | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_vmmisc.c $(filter-out src/main.o,$(OBJ)) $(LDFLAGS) -lm

clean-vmmisc:
	-rm -f $(VMMISC_TEST)

.PHONY: test_vmmisc clean-vmmisc

# ---------------------------------------------------------------------------
# null / type-0 值模型（pushfalse → FVP_NULL）
#   make test_nullvalue
# ---------------------------------------------------------------------------
NULLVALUE_TEST = bin/test_nullvalue$(EXE)

test_nullvalue: $(NULLVALUE_TEST)

$(NULLVALUE_TEST): tests/test_nullvalue.c src/vm.c src/vm.h src/hcb.c src/fvp.c $(OBJ) | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_nullvalue.c $(filter-out src/main.o,$(OBJ)) $(LDFLAGS) -lm

clean-nullvalue:
	-rm -f $(NULLVALUE_TEST)

.PHONY: test_nullvalue clean-nullvalue

# ---------------------------------------------------------------------------
# make check —— 一键构建并运行**不需要游戏数据**的全部单元测试。
# CI（.github/workflows/ci.yml）直接跑这个，本地也一样：
#   make check
# 需要游戏数据的验收（compare.py / verify_*.ps1 / run_all.py）不在这里。
# ---------------------------------------------------------------------------
# 不在这里的两个：test_gfx 要 gfx-scene.json（由 verify_gfx.ps1 提供），
# test_snd 要一个 ogg/wav（游戏数据或 work/ 里的样本，都不入库）。
UNIT_TESTS = $(SAVE_TEST) $(SAVEVM_TEST) $(FLAGS_TEST) \
             $(TEXTATTR_TEST) $(THUMB_TEST) $(GFX_MOTION_TEST) \
             $(TIMER_TEST) $(TYPEWRITER_TEST) $(TEX_CACHE_TEST) $(HISTORY_TEST) \
             $(PRIM_HIT_TEST) $(GLYPH_FALLBACK_TEST) $(MOTION_TEST) \
             $(CONTROL_TEST) $(GRAPHCOLOR_TEST) $(TEXTBUFF_TEST) \
             $(GAIJI_TEST) $(GAIJI_RENDER_TEST) $(TEXTPRIMS_TEST) \
             $(VMMISC_TEST) $(NULLVALUE_TEST)

check: $(UNIT_TESTS)
	@set -e; for t in $(UNIT_TESTS); do \
	    echo "== $$t =="; $$t; \
	done; echo "make check: PASS"

.PHONY: check
