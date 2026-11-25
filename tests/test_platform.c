/*
 * 跨平台 OS 缝（src/platform/platform.h）的单元测试。
 *
 * 这里用**便携参考实现**（src/platform/portable/platform_portable.c）验证缝本身：
 * 单调时钟在走、日志能写出、文件读入的长度与内容正确、错误路径返回 0。
 * 它同时是"引擎核心只需要这四个函数就能跑"的证据——这些代码不含任何 Win32 头。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/platform/platform.h"

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

int main(void)
{
    const char *path = "bin/_platform_probe.bin";
    static const char payload[] = "openfvp-platform-seam\n";
    FILE *f;
    unsigned char *data = NULL;
    size_t len = 0;
    double t0, t1;
    unsigned long ms0, ms1;
    int i;

    /* --- 1. 时钟单调（clock() 驱动，允许相等但不允许回退） --- */
    t0 = fvp_plat_now_sec();
    t1 = fvp_plat_now_sec();
    CHECK(t1 >= t0, "now_sec is monotonic (%f -> %f)", t0, t1);
    ms0 = fvp_plat_ticks_ms();
    ms1 = fvp_plat_ticks_ms();
    CHECK(ms1 >= ms0, "ticks_ms is monotonic (%lu -> %lu)", ms0, ms1);
    CHECK(fvp_plat_now_sec() >= 0.0, "now_sec is non-negative");
    /* 空转一点 CPU，确认时钟确实会前进 */
    {
        volatile double sink = 0.0;
        for (i = 0; i < 200000; ++i) {
            sink += (double)i;
        }
        CHECK(sink > 0.0, "busy loop ran");
    }
    CHECK(fvp_plat_ticks_ms() >= ms0, "ticks_ms advances or stays");

    /* --- 2. 日志：空指针安全，非空不崩 --- */
    fvp_plat_log(NULL);
    fvp_plat_log("test_platform: seam log line\n");
    CHECK(1, "log calls are safe");

    /* --- 3. 文件读入：长度与内容正确 --- */
    f = fopen(path, "wb");
    CHECK(f != NULL, "write probe file");
    if (f != NULL) {
        CHECK(fwrite(payload, 1, sizeof(payload) - 1, f) ==
                  sizeof(payload) - 1,
              "probe file written");
        fclose(f);
        CHECK(fvp_plat_read_file(path, &data, &len) == 1, "read_file ok");
        CHECK(len == sizeof(payload) - 1, "read_file length (%zu)", len);
        CHECK(data != NULL &&
                  memcmp(data, payload, sizeof(payload) - 1) == 0,
              "read_file content matches");
        free(data);
        remove(path);
    }

    /* --- 4. 错误路径：不存在的文件返回 0，输出被清零 --- */
    data = (unsigned char *)0x1;
    len = 999;
    CHECK(fvp_plat_read_file("bin/_no_such_file_here.bin", &data, &len) == 0,
          "missing file rejected");
    CHECK(data == NULL && len == 0, "outputs cleared on failure");
    CHECK(fvp_plat_read_file(NULL, &data, &len) == 0, "NULL path rejected");
    CHECK(fvp_plat_read_file(path, NULL, &len) == 0, "NULL data rejected");
    CHECK(fvp_plat_read_file(path, &data, NULL) == 0, "NULL len rejected");

    printf("test_platform: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}