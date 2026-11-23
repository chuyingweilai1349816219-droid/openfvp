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

OBJ      = $(CORE_SRC:.c=.o) $(SAVE_OBJ) $(SAVEVM_OBJ) $(FLAGS_OBJ) \
           $(TEXTATTR_OBJ) $(THUMB_OBJ)
LIB_OBJ  = $(SRC:.c=.o) $(SND_SRC:.c=.o) $(RES_SRC:.c=.o) $(SAVE_OBJ) \
           $(SAVEVM_OBJ) $(FLAGS_OBJ) $(TEXTATTR_OBJ) $(THUMB_OBJ)

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
               src/save.c src/save_vm.c src/flags.c src/textattr.c src/thumb.c
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
