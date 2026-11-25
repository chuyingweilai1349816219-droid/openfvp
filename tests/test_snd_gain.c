/*
 * 音频三级音量（槽 × 主 × 类型）增益链的单元测试。
 *
 * 背景：命令表里音量分三层——`SoundVol(107)` 槽音量、`SoundMasterVol(101)`
 * 主音量、`SoundTypeVol(106)` 按类型（BGM/语音/SE）音量，外加通道静音
 * （`AudioSilentOn` / `SoundSilentOn`）。真实生效的增益是三者乘积，之前这条
 * 链只写在 syscall.c 的静态函数里，没法单测；这轮抽成纯函数
 * `fvp_snd_gain_calc()`，这里把它钉死（不依赖音频设备/解码器）。
 */
#include <stdio.h>
#include <string.h>

#include "../src/vm.h"
#include "../src/snd.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            ++g_pass;                                                         \
        } else {                                                              \
            ++g_fail;                                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

static int near(float a, float b)
{
    float d = a - b;

    return (d < 0 ? -d : d) < 1e-6f;
}

int main(void)
{
    float g;

    /* --- 1. 全默认（100/100/100）即 1.0 --- */
    CHECK(near(fvp_snd_gain_calc(100, 100, 100, 0), 1.0f),
          "default is unity (%.4f)", fvp_snd_gain_calc(100, 100, 100, 0));

    /* --- 2. 单层减半 --- */
    CHECK(near(fvp_snd_gain_calc(50, 100, 100, 0), 0.5f), "slot 50 -> 0.5");
    CHECK(near(fvp_snd_gain_calc(100, 50, 100, 0), 0.5f), "master 50 -> 0.5");
    CHECK(near(fvp_snd_gain_calc(100, 100, 50, 0), 0.5f), "type 50 -> 0.5");

    /* --- 3. 三层相乘 --- */
    CHECK(near(fvp_snd_gain_calc(50, 50, 50, 0), 0.125f),
          "50*50*50 -> 0.125 (%.4f)", fvp_snd_gain_calc(50, 50, 50, 0));
    CHECK(near(fvp_snd_gain_calc(25, 80, 50, 0), 0.1f),
          "25*80*50 -> 0.1 (%.4f)", fvp_snd_gain_calc(25, 80, 50, 0));
    CHECK(near(fvp_snd_gain_calc(75, 60, 40, 0), 0.18f),
          "75*60*40 -> 0.18 (%.4f)", fvp_snd_gain_calc(75, 60, 40, 0));

    /* --- 4. 任一层为 0 即静音 --- */
    CHECK(fvp_snd_gain_calc(0, 100, 100, 0) == 0.0f, "slot 0 -> 0");
    CHECK(fvp_snd_gain_calc(100, 0, 100, 0) == 0.0f, "master 0 -> 0");
    CHECK(fvp_snd_gain_calc(100, 100, 0, 0) == 0.0f, "type 0 -> 0");

    /* --- 5. 通道静音优先于一切 --- */
    CHECK(fvp_snd_gain_calc(100, 100, 100, 1) == 0.0f, "silent -> 0");
    CHECK(fvp_snd_gain_calc(50, 50, 50, 1) == 0.0f, "silent beats any volume");

    /* --- 6. 越界夹到 [0,1] --- */
    CHECK(near(fvp_snd_gain_calc(200, 100, 100, 0), 1.0f),
          "slot >100 clamps to 1 (%.4f)", fvp_snd_gain_calc(200, 100, 100, 0));
    CHECK(near(fvp_snd_gain_calc(100, 150, 100, 0), 1.0f),
          "master >100 clamps to 1");
    CHECK(fvp_snd_gain_calc(-5, 100, 100, 0) == 0.0f, "negative -> 0");
    CHECK(near(fvp_snd_gain_calc(120, 120, 100, 0), 1.0f),
          "two layers >100 clamp to 1");

    /* --- 7. 单调性：任一层变大，增益不变小 --- */
    {
        float prev = 0.0f;
        int v;

        for (v = 0; v <= 100; v += 10) {
            g = fvp_snd_gain_calc(v, 80, 60, 0);
            CHECK(g >= prev, "monotonic in slot vol at %d (%.4f >= %.4f)", v,
                  g, prev);
            prev = g;
        }
    }
    {
        float prev = 0.0f;
        int v;

        for (v = 0; v <= 100; v += 10) {
            g = fvp_snd_gain_calc(80, v, 60, 0);
            CHECK(g >= prev, "monotonic in master vol at %d", v);
            prev = g;
        }
    }

    /* --- 8. 分档语义：主音量影响所有类型，类型音量只影响自己那组 --- */
    CHECK(near(fvp_snd_gain_calc(100, 60, 100, 0), 0.6f),
          "master 60 affects a slot with default type");
    CHECK(near(fvp_snd_gain_calc(100, 100, 60, 0), 0.6f),
          "type 60 affects only its own group");

    /* --- 9. 桥上的主音量 setter：夹紧 + 立即生效 + 可读回 --- */
    {
        Snd *snd = snd_create(8000, 2);
        FvpVM vm;

        memset(&vm, 0, sizeof vm);
        CHECK(snd != NULL, "snd_create for the bridge test");
        if (snd != NULL) {
            fvp_vm_attach_snd(&vm, snd);
            CHECK(vm.snd_bridge != NULL, "bridge attached");
            CHECK(fvp_snd_bridge_master(vm.snd_bridge) == 100,
                  "default master is 100 (got %d)",
                  fvp_snd_bridge_master(vm.snd_bridge));
            CHECK(fvp_snd_bridge_set_master(vm.snd_bridge, 50) == 50,
                  "set master 50");
            CHECK(fvp_snd_bridge_master(vm.snd_bridge) == 50,
                  "read back 50 (got %d)",
                  fvp_snd_bridge_master(vm.snd_bridge));
            CHECK(fvp_snd_bridge_set_master(vm.snd_bridge, 150) == 100,
                  "150 clamps to 100 (got %d)",
                  fvp_snd_bridge_set_master(vm.snd_bridge, 150));
            CHECK(fvp_snd_bridge_set_master(vm.snd_bridge, -5) == 0,
                  "negative clamps to 0 (got %d)",
                  fvp_snd_bridge_set_master(vm.snd_bridge, -5));
            CHECK(fvp_snd_bridge_set_master(vm.snd_bridge, 80) == 80,
                  "set master 80");
            CHECK(fvp_snd_bridge_set_master(NULL, 30) == -1,
                  "NULL bridge returns -1");
            CHECK(fvp_snd_bridge_master(NULL) == 100,
                  "NULL bridge reports the default");
            fvp_vm_attach_snd(&vm, NULL);
            CHECK(vm.snd_bridge == NULL, "bridge detached");
            snd_destroy(snd);
        }
    }

    printf("test_snd_gain: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}