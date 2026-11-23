# FVP 引擎（C 实现）
#
#   make            构建 bin/fvm.exe（脚本 VM，已通过逐字节等价验证）
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

OBJ      = $(CORE_SRC:.c=.o)
LIB_OBJ  = $(SRC:.c=.o) $(SND_SRC:.c=.o) $(RES_SRC:.c=.o)

BIN = bin/fvm$(EXE)
LIB = bin/libfvp.a

HEADERS = src/fvp.h src/encoding.h src/hcb.h src/vm.h src/syscall.h \
          src/snd.h src/bin.h src/inflate.h src/hzc1.h \
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
#   make win32                   构建 bin\fvp.exe
#   make win32 GFX_FORCE_STUB=1  强制链接 tests/stub_gfx.c 的测试图案
#
# 只追加本节，不改变 all / lib / test 的语义。src/gfx.c 一旦落地就自动编进
# 壳里；在那之前用 tests/stub_gfx.c，保证窗口/帧循环/输入/音频可以先验证。
# ---------------------------------------------------------------------------
WIN32_DIR   = src/platform/win32
WIN32_SHELL = $(WIN32_DIR)/main_win32.c $(WIN32_DIR)/win32_window.c \
              $(WIN32_DIR)/win32_audio.c $(WIN32_DIR)/win32_log.c
WIN32_CORE  = src/fvp.c src/encoding.c src/hcb.c src/vm.c src/syscall.c src/snd.c
WIN32_RES   = src/bin.c src/hzc1.c src/inflate.c
WIN32_GFX_H = $(wildcard src/gfx.h)

ifneq ($(GFX_FORCE_STUB),1)
ifneq ($(wildcard src/gfx.c),)
WIN32_GFX_C   = src/gfx.c
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
GFX_OBJ  = src/gfx.o src/bin.o src/hzc1.o src/inflate.o
GFX_LIB  = bin/libfvp_gfx.a
GFX_TEST = bin/test_gfx$(EXE)

gfx: $(GFX_LIB)

$(GFX_LIB): $(GFX_OBJ) | bin
	$(AR) rcs $@ $(GFX_OBJ)

test_gfx: $(GFX_TEST)

$(GFX_TEST): tests/test_gfx.c src/gfx.h $(GFX_OBJ) | bin
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_gfx.c $(GFX_OBJ) $(LDFLAGS) -lm

.PHONY: gfx test_gfx

clean-gfx:
	-rm -f $(GFX_OBJ) $(GFX_LIB) $(GFX_TEST)

.PHONY: clean-gfx