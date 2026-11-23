# Android 外壳（fvp-android）

这一版的目标不是画面，而是把「**能装、能启动、能显示一帧、能收到触摸、能退出**」这条链路跑通：
APK 构建流水线、JNI 桥、原生窗口（EGL/ANativeWindow）、触摸输入、资产目录。

引擎本体（`fvp/src/`）**一行未改**：外壳用自己的适配层 `jni/fvp_android_glue.c` 包住它。
渲染内容是**占位**（彩色测试图案 + HUD 文本），真正的图层合成是后续工作。

```
fvp-android/
  AndroidManifest.xml          包名 com.openfvp.saclet / 0.1.0 / minSdk 26 / targetSdk 34 / 横屏全屏
  build.ps1                    一键出 out\fvp-0.1.0.apk（不用 Gradle）
  src/com/openfvp/saclet/
    MainActivity.java          Activity + SurfaceView + 渲染线程 + 触摸/按键 + HUD 位图
  jni/
    native.c                   JNI 桥 / 日志与崩溃报告 / EGL 渲染器
    fvp_android_glue.c         引擎适配：载入 .hcb、按帧推进、文本与输入转发
    fvp_android_glue.h         适配层接口（不含任何 Android 头，可在桌面编译测试）
  tools/
    host_test.c                桌面侧冒烟测试（用同一份 glue 跑真实 .hcb）
    host-test.ps1              编 + 跑上面这个冒烟测试的一条命令
    device-test.ps1            装 APK / 建目录 / 推数据 / 启动 / 点一下 / 截图 一条龙
  screenshots/
    device-1.png               模拟器上启动后的第一帧
    device-2.png               点一下之后：触摸十字 + HUD 计数
    device-logcat.txt          设备验证时的 logcat 副本
    device-last-run.log        设备验证时的 files/last-run.log 副本
    device-exit-log.txt        音量键 + 返回键退出的日志副本
  out/
    fvp-0.1.0.apk              产物
    lib/<abi>/libfvp.so        各 ABI 的原生库（构建中间产物，留作检查）
  build/                       中间文件（classes、dex、对齐前后的 apk）
```

---

## 1. 构建

### 依赖（全部来自现有 SDK/NDK，不联网、不装 Gradle）

| 组件 | 路径 |
|---|---|
| Android SDK | `C:\Users\Mechrevo\android-dev\sdk`（`-Sdk` 或用 `ANDROID_SDK_ROOT` 覆盖） |
| build-tools | `34.0.0`（aapt2 / zipalign / apksigner / d8） |
| platform | `platforms\android-34\android.jar` |
| NDK | `ndk\26.1.10909125`（clang 包装器 `aarch64-linux-android26-clang.cmd` / `x86_64-linux-android26-clang.cmd`） |
| JDK | **优先** `tools\android\jdk\jdk-17.0.21+9`，没有才退回 `JAVA_HOME` |

> JDK 必须是 17：build-tools 34 里的 d8（R8 8.2）解析 javac 21 生成的 class 会内部 NPE
> （`Cannot invoke "String.length()" because "<parameter1>" is null`）。`build.ps1` 已经优先选
> 工作区里的 JDK 17。

### 一条命令

```powershell
powershell -ExecutionPolicy Bypass -File fvp-android\build.ps1
# 只出 arm64（快一半）：
powershell -ExecutionPolicy Bypass -File fvp-android\build.ps1 -Abi arm64-v8a
```

产物：`fvp-android\out\fvp-0.1.0.apk`（arm64-v8a + x86_64 双 ABI，约 0.20 MB / 213,530 字节）。

构建流水线（和 `android-app\build.ps1` 同一套路，不用 Gradle）：

1. **NDK clang** → `out\lib\<abi>\libfvp.so`
   `fvp/src/{fvp,encoding,hcb,vm,syscall}.c` + `jni/native.c` + `jni/fvp_android_glue.c`，
   `-shared -fPIC -O2 -std=c99 -llog -landroid -lEGL -lGLESv2 -lm`，然后 `llvm-strip --strip-unneeded`。
   `fvp/src/main.c` 不编进去：它有自己的 `main()` 和命令行参数解析，外壳提供入口。
   **这五个引擎文件是写死的列表，不是 `*.c` 通配**：`fvp/src` 是另一个 hand 的活动工作树，
   正在往里加 `gfx.c` / `hzc1.c` / `snd.c` / `bin.c` 这类渲染和资产模块，它们这版外壳用不到、
   也还不保证能编到 NDK（`gfx.c` 在写这份文档时正处于改到一半的状态）。通配会把它们拖进
   `libfvp.so`，并在任何一次对方半成品保存时把外壳的构建打挂。
   每个 ABI 都打印 `llvm-readelf -h` 的 `Machine:` 行，机器不对就直接失败。
2. **aapt2 link** → `build\base.apk`（无 res/assets，`--min-sdk-version 26 --target-sdk-version 34`）
3. **javac**（`-source 8 -target 8`）→ `jar` → **d8**（`--min-api 26`）→ `classes.dex`
4. **组装**：把 `classes.dex`、`lib/<abi>/libfvp.so` 塞进 `base.apk`
5. **zipalign -f 4** → **apksigner**（首次运行会用 keytool 在工作区里生成 `debug.keystore`，口令 `android`）→ `verify`
6. 最后打印 `aapt2 dump badging` 的关键行、APK 大小、每个 ABI 的 ELF 机型

### 安装与运行

```powershell
$adb = "C:\Users\Mechrevo\android-dev\sdk\platform-tools\adb.exe"
$pkg = "com.openfvp.saclet"
$game = "/storage/emulated/0/Android/data/$pkg/files/game"

# 装
& $adb install -r fvp-android\out\fvp-0.1.0.apk

# 先启动一次：<files>/game 必须由 app 自己创建（shell 创建的目录 app 进不去），
# 这一步之后目录属于 app，再 push 才有意义。
& $adb shell am start -n "$pkg/.MainActivity"
Start-Sleep 5
& $adb shell am force-stop $pkg

# 推脚本（目录就是 getExternalFilesDir(null)/game，不需要任何存储权限）
& $adb push fvp\out\cn\iroseka_zh.hcb "$game/iroseka.hcb"
Set-Content -Path fvp-android\build\encoding.txt -Value "gbk" -Encoding ASCII
& $adb push fvp-android\build\encoding.txt "$game/encoding.txt"
& $adb shell "chmod 644 $game/*"      # adb push 的文件要是 other-readable

# 起 + 看日志
& $adb shell am start -W -n "$pkg/.MainActivity"
& $adb logcat -s openfvp:V
& $adb shell run-as $pkg cat files/last-run.log     # debuggable=true 才能这么读

# 退出：返回键 → onDestroy → nativeShutdown
& $adb shell input keyevent 4
```

也可以直接用脚本做上面全部步骤（装、建目录、推数据、启动、点一下、截图）：

```powershell
powershell -ExecutionPolicy Bypass -File fvp-android\tools\device-test.ps1 `
    -Serial emulator-5554 -Data fvp\out\cn\iroseka_zh.hcb
```

没有脚本也能启动：外壳照常渲染测试图案，HUD 第二行写 `engine: NOT STARTED | script not found: <路径>`。

---

## 2. JNI 接口

Java：`com.openfvp.saclet.MainActivity`，库名 `fvp`（`System.loadLibrary("fvp")` → `libfvp.so`）。

| Java 声明 | native 行为 | 返回 |
|---|---|---|
| `long nativeInit(String dataDir, String assetDir)` | `dataDir` = 游戏目录（找 `iroseka.hcb`）；`assetDir` = 日志目录（`files/`）。载入脚本、起 VM | 引擎句柄（现在是 `1`；`0` = 脚本缺失/载入失败，外壳继续跑） |
| `int nativeStep()` | 给 VM 追加一帧的指令预算并执行 | `1` 运行中，`0` 结束/未启动 |
| `int nativeRender(Surface s, int w, int h)` | 用 EGL 画一帧到 Surface | `1` 成功，`0` 还没准备好（重试），`-1` 致命 |
| `void nativeTouch(int action, float x, float y)` | 转发触摸（`action` 是 `MotionEvent` 的 masked action） | — |
| `void nativeSetOverlay(byte[] rgba, int w, int h)` | 接收 Java 画好的 HUD 位图（RGBA8888，POT 尺寸），渲染线程下一帧上传成纹理 | — |
| `String nativeStatus()` | HUD 的 4 行文本（引擎步数、文本行数、触摸/按键、最新一行脚本文本） | 文本 |
| `void nativeKey(int keyCode, int down)` | 音量键/返回键，记录并写日志 | — |
| `void nativeShutdown()` | 停 VM、释放 EGL/Surface | — |

`JNI_OnLoad` 里缓存 `JavaVM`（现在只用于日志/未来回调用）并安装致命信号处理器。
日志统一走 `__android_log_print`（tag `openfvp`），同时写 `<files>/last-run.log`；
致命信号写 `<files>/crash.log`（信号、fault 地址、`_Unwind_Backtrace` 返回地址）。

### 渲染线程

`MainActivity` 用一个 Java 线程（`fvp-render`）跑 60 Hz 循环：
`nativeStep()` → `nativeRender(surface, w, h)`，每 200 ms 用 `nativeStatus()` 重画 HUD 位图并 `nativeSetOverlay()`。
`SurfaceHolder` 回调只管 Surface 生命周期；`onPause` 停循环，`onResume` 在 Surface 还在时重启，
`onDestroy` 先 `join` 渲染线程再 `nativeShutdown()`（GL 只在一个线程上碰）。

---

## 3. 引擎接入

`fvp/src/` 是 batch 程序：`fvp_hcb_load` → `fvp_vm_init` → `fvp_vm_start` → `fvp_vm_run(vm, N)` → 打印文本。
适配层把它变成帧循环：

- **载入**：`<dataDir>/iroseka.hcb`，`stat` 之后 `fvp_hcb_load`。编码默认 `cp932`（日文原版）；
  `<dataDir>/encoding.txt` 里写 `gbk` 就走 GBK（中文重制版）。选定的编码载入失败时自动换另一种再试一次。
- **一帧 = 一段指令预算**：`fvp_vm_run` 的第二个参数是**累计**预算，所以
  `budget += 5000; fvp_vm_run(&vm, budget)` 就是「再跑 5000 条指令」。
  `<dataDir>/steps.txt` 可以改这个数（100..500000），方便在设备上调，不用重编。
- **文本**：VM 的 `vm->text[]` 只增不减，取最新一行的 `FvpStr` 复制进 HUD（quiet=1，不往 stdout 写）。
- **触摸**：`nativeTouch` 把 view 像素换算到引擎的 1920×1080 画布写进 `vm->cx` / `vm->cy`；
  `ACTION_DOWN` 时让下一帧的 `vm->click_period = 2`——引擎现有的 `InputGetDown` 语义是
  `steps % click_period < 2`，周期 2 意味着一帧「按下」、下一帧「松开」，等于用引擎自己的输入路径
  送出一次点击，而不是绕过去改内部状态。
- **按键**：记录 + 日志 + HUD（引擎的 `key_mask` 目前是占位实现，没有真实按键语义可接）。

---

## 4. 目录约定

```
/sdcard/Android/data/com.openfvp.saclet/files/     <- getExternalFilesDir(null)
    game/                                          <- nativeInit 的 dataDir
        iroseka.hcb                                这个版本固定找这个名字
        encoding.txt                               可选：gbk / cp932（必须 644）
        steps.txt                                  可选：每帧指令预算
    last-run.log                                   本次运行日志（每次启动清空）
    crash.log                                      上次致命信号留下的报告（有才写）
/data/data/com.openfvp.saclet/files/               <- getFilesDir()，nativeInit 的 assetDir
```

`getExternalFilesDir` 在 `adb push` 之前可能不存在，而且**必须由 app 自己创建**：
用 `adb shell mkdir` 建的目录属于 shell，app 连 `stat` 都进不去，会一直显示
`script not found`。先启动一次 app（它自己 `mkdirs()`），再 push。APK 里**不打包**游戏数据。

---

## 5. 屏幕上看到什么

占位渲染（`jni/native.c` 的 `DrawPattern` / `DrawOverlay`），全部在渲染线程用 EGL + GLES2 画：

- 顶部 1/3：8 条彩条 + 一条白色扫光（扫光在动 = 没卡死）
- 中下部：网格
- 最底：帧心跳条，每 240 帧填满重来
- 触摸落点：青色十字 + 黄色角标（证明触摸真的到了 native）
- 底部 HUD（Java 画进 Bitmap → 上传成纹理）：4 行文本，含 `steps=`、`texts=`、`frame=`、
  `touch=`、`keys=`，最后一行是引擎最新一句脚本文本（系统字体，能显示中日文）

HUD 用 Java 画是刻意的：native 侧此时没有字形表，而系统字体是唯一能显示 CJK 的字体来源；
等引擎有了自己的文本渲染，这块会被替换掉。

---

## 6. 已验证

### 6.1 构建（本机，2026-11-23；从空 `build` 目录完整重建过一次）

```
ABI      arm64-v8a, x86_64
== clang (arm64-v8a) ==
   arm64-v8a  Machine: AArch64  410.7 KB
== clang (x86_64) ==
   x86_64     Machine: Advanced Micro Devices X86-64  412.0 KB
...
APK      fvp-android\out\fvp-0.1.0.apk
size     0.2 MB
```

`llvm-readelf -h out\lib\arm64-v8a\libfvp.so`：

```
Class:    ELF64
OS/ABI:   UNIX - System V
Type:     DYN (Shared object file)
Machine:  AArch64
```

`llvm-nm -D --defined-only` 里 8 个 JNI 入口 + `JNI_OnLoad` 都是 `T`（已导出），
名字与 Java 声明逐个对上（`Java_com_openfvp_saclet_MainActivity_nativeInit` 等）。

`aapt2 dump badging`：

```
package: name='com.openfvp.saclet' versionCode='1' versionName='0.1.0' compileSdkVersion='34'
sdkVersion:'26'
targetSdkVersion:'34'
application: label='openfvp' icon=''
application-debuggable
launchable-activity: name='com.openfvp.saclet.MainActivity'
native-code: 'arm64-v8a' 'x86_64'
```

`apksigner verify` 通过（`CN=Android Debug, O=Android, C=US`，signer SHA-256
`b8332ff5bd194f4085cf7e314170e14dc91356991b973b566f311a422c642988`）。
APK 内容：`classes.dex` + `lib/arm64-v8a/libfvp.so` + `lib/x86_64/libfvp.so` + 签名。

### 6.2 引擎适配层（桌面冒烟测试）

`jni/fvp_android_glue.c` 不含 Android 头，所以同一份代码可以用工作区的 w64devkit gcc 在桌面上跑
（`tools/host_test.c`，一条命令：`powershell -ExecutionPolicy Bypass -File fvp-android\tools\host-test.ps1`，
真实脚本 `fvp\out\cn\iroseka_zh.hcb`，GBK）：

```
init(.../game) -> 1
--- after steps ---
openfvp 0.1.0 | android shell | libfvp.so loaded
engine: running
steps=300000 (+5000/frame) texts=55 frame=60 vm=live
touch=0 (0,0) act=0 | keys=0 last=0/0 | text:  真红／悠马
step after touch -> 1
--- after touch + key ---
steps=305000 (+5000/frame) texts=56 frame=61 vm=live
touch=1 (480,270) act=0 | keys=1 last=24/1 | text: 为什么呢？
shutdown done
```

即：3.7 MB 的 `.hcb` 载入成功、60 帧 30 万步、55 行文本、触摸与按键转发、干净退出。

### 6.3 设备上跑起来了（Android 14 x86_64 模拟器）

真机没在线（`adb devices` 为空），所以用一个 **Android 14 / API 34 / x86_64 模拟器**
（Pixel 5 配置，2340×1080，GL 走 SwiftShader 软渲染）跑的完整链路。`arm64-v8a` 的产物只做了静态校验，
没有在 arm 真机上启动过。

安装：`adb install -r` → `Success`。

启动日志（`adb logcat -s openfvp:V`，完整副本在 `fvp-android\screenshots\device-logcat.txt`）：

```
I openfvp : JNI_OnLoad: libfvp.so 0.1.0 loaded
I openfvp : openfvp android shell 0.1.0 starting
I openfvp : nativeInit: dataDir=/storage/emulated/0/Android/data/com.openfvp.saclet/files/game assetDir=/data/user/0/com.openfvp.saclet/files
I openfvp : nativeInit: engine started
I openfvp : nativeInit status:
             openfvp 0.1.0 | android shell | libfvp.so loaded
             engine: running
             steps=0 (+5000/frame) texts=0 frame=0 vm=live
             touch=0 (0,0) act=0 | keys=0 last=0/0 | text:
I openfvp : nativeInit -> 1 in 176 ms; .../game/iroseka.hcb
I openfvp : surfaceCreated
I openfvp : render thread started
I openfvp : surfaceChanged 2340x1080
I openfvp : nativeRender: new Surface
I openfvp : surface 2340x1080 (view reported 2340x1080)
I openfvp : GL: OpenGL ES 3.0 (OpenGL ES 3.0 SwiftShader 4.0.0.1) | Android Emulator OpenGL ES Translator (Google SwiftShader)
I openfvp : nativeRender: first frame 2340x1080
I openfvp : HUD texture 2048x192 uploaded; rows 63 185 80 255 / 13 17 23 255 (accent / background)
```

`files/last-run.log` 同时生成在 `run-as` 可读的目录里（同一份内容，副本
`fvp-android\screenshots\device-last-run.log`）。

链路逐项确认：

| 目标 | 证据 |
|---|---|
| 能装 | `adb install -r` → `Success`；`pm list packages` 里有 `com.openfvp.saclet` |
| 能启动 | `nativeInit -> 1 in 176 ms`，脚本 3.7 MB 载入成功（`encoding.txt=gbk`） |
| 能显示一帧 | `nativeRender: first frame 2340x1080`，GL ES 3.0；截图见下 |
| 引擎活着 | HUD：`steps=31635000 (+5000/frame) texts=1170 frame=6327 vm=live` |
| 能收到触摸 | `touch=2 (640,400) act=1`，十字准线画在落点；HUD 的 `touch=` 计数递增 |
| 引擎文本上屏 | HUD 最后一行：`text: 而是一个少女，从空中落了下来。` |
| 音量键 | `input keyevent 24` → `key: code=24 down=1` / `down=0` |
| 能退出 | `input keyevent 4` → `back key: leaving` → `render thread stopped (paused)` → `surfaceDestroyed` → `onDestroy: shutting the engine down` → `nativeShutdown: done`；launcher 回到前台（副本 `fvp-android\screenshots\device-exit-log.txt`） |

截图（模拟器 2340×1080，横屏；图案是占位渲染，HUD 是 Java 画的位图）：

- ![启动后第一帧](</C:/Users/Mechrevo/Documents/deepseek-harness/default-workspace/fvp-android/screenshots/device-1.png>)
- ![点一下之后：触摸十字与 HUD 计数](</C:/Users/Mechrevo/Documents/deepseek-harness/default-workspace/fvp-android/screenshots/device-2.png>)

一点模拟器环境的说明：SwiftShader 软渲染下这颗 AVD 很慢（本机也没有真机），启动过程中
系统自己弹过 `Process system isn't responding`（system_server 的 ANR，与本 app 无关），
点 `Wait` 之后继续。真机上不会遇到这个。

---

## 7. 已知限制

1. **没有游戏画面**。渲染是测试图案 + HUD；HZC1 图层的解码/合成/混合还没接进来。
2. **文本只在 HUD 里**。native 侧没有字形表（引擎自己的字体层还没有），GL 里不直接画文本。
3. **输入是退化实现**。引擎的 input syscall 本身是占位（`InputGetDown` 周期点击、cursor 取
   `vm->cx/cy`），所以触摸只能表达「一次点击 + 坐标」；多点、滑动、长按、滚轮都没有语义，
   音量/返回键只记录。等引擎有真实输入接口再接。
4. **不是真正的逐帧 tick**。`fvp_vm_run` 按指令预算推进，没有 vsync 对齐；Java 侧用 60 Hz sleep 限速，
   一帧 5000 步在桌面/模拟器上远不到一帧的时间（900k 步 ≈ 0.05 s），预算大小用 `steps.txt` 调。
5. **`fvp_fatal` 会直接退进程**（`exit(2)`：OOM、字符串拼接超限）。现场会留在 `last-run.log`，
   原生崩溃另有 `crash.log`；外壳不拦截它。
6. **只有 arm64-v8a / x86_64**，没编 armeabi-v7a。
7. **没有音频、没有存档目录抽象**：脚本里碰文件的 syscall 仍是空实现，目录约定只用于 `.hcb`。
8. **只认 `iroseka.hcb` 一个文件名**（按本版要求固定），多脚本/选择菜单还没有。
9. **HUD 位图固定 2048×192 POT**、按屏幕宽度等比缩放，超长文本行会被 Canvas 裁掉。
10. **APK 不含游戏数据**（3.7 MB 的脚本也由用户推入 `files/game/`）。

---

## 8. 排错

| 现象 | 查什么 |
|---|---|
| 黑屏但有日志 | `nativeRender` 返回 0：EGL 还没建好（Surface 刚创建）；日志里有 `eglCreateWindowSurface failed` / `eglMakeCurrent failed` |
| 启动就退出 | `adb logcat -s AndroidRuntime:V openfvp:V`；`files/crash.log`；`run-as` 读 `files/last-run.log` |
| HUD 显示 `script not found` 但文件明明在 | 看 `ls -ld .../files/game` 的 owner：应该是 `u0_aXXX`，不是 `shell`。用 `adb shell mkdir` 建的目录属于 shell，app 连 `stat` 都进不去；先启动一次 app 让它建目录，再 push |
| 文本乱码 | 换 `encoding.txt`（日文 `cp932`，中文重制 `gbk`）；它必须是 644，否则 app 读不到、退回默认 cp932，中文就成片假名 |
| HUD 背景色发蓝 / 红蓝互换 | HUD 位图必须按 `getPixels()` 的 ARGB 拆成 RGBA；直接 `copyPixelsToBuffer` 得到的字节序是实现相关的（这里踩过：背景 #0D1117 变成 (17,23,255)） |
| 构建时 d8 报 NPE | JDK 不是 17；删掉 `JAVA_HOME` 或改用 `tools\android\jdk\jdk-17.0.21+9` |
| 构建时 clang 找不到 | `-Ndk` 指定 NDK，或确认 `ndk\26.1.10909125\toolchains\llvm\prebuilt\windows-x86_64\bin` |
| 模拟器里 EGL 起不来 | 用 `-gpu swiftshader_indirect`；真机不会遇到 |
| 一进全屏就弹 `Viewing full screen` | 系统的沉浸式提示（一次性的），点 `Got it` 或 `adb shell settings put secure immersive_mode_confirmations confirmed` |
| 模拟器整体 ANR / 极慢 | SwiftShader 软渲染 + 1080p 全屏每帧多次 clear 很吃 CPU；真机不会遇到。可以临时调小 `steps.txt` |

`adb shell run-as com.openfvp.saclet cat files/last-run.log` 能用是因为 Manifest 里
`android:debuggable="true"`（开发外壳）。要出正式包时去掉它。