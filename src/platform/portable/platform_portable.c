/* 便携参考实现：只用 C 标准库实现 src/platform/platform.h。
 *
 * 存在的意义有两个：
 *   1. 给单测一个**不依赖 Win32** 的实现（tests/test_platform.c 顺手证明"核心
 *      只需要这四件事就能跑起来"）；
 *   2. 给将来移植（Linux/Android 的 headless 构建、CI 里的无窗口跑测）一个起点。
 *
 * 注意：帧循环/窗口/音频输出仍然由各平台外壳自己实现——它们不在最小缝里。
 */
#include "../platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef CLOCKS_PER_SEC
#define CLOCKS_PER_SEC 1000000
#endif

double fvp_plat_now_sec(void)
{
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

unsigned long fvp_plat_ticks_ms(void)
{
    return (unsigned long)((double)clock() * 1000.0 / (double)CLOCKS_PER_SEC);
}

void fvp_plat_log(const char *utf8)
{
    if (utf8 != NULL) {
        fputs(utf8, stderr);
    }
}

int fvp_plat_read_file(const char *path, unsigned char **data, size_t *len)
{
    FILE *f;
    long n;
    unsigned char *buf;

    if (path == NULL || data == NULL || len == NULL) {
        return 0;
    }
    *data = NULL;
    *len = 0;
    f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    buf = (unsigned char *)malloc((size_t)n);
    if (buf == NULL) {
        fclose(f);
        return 0;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    *data = buf;
    *len = (size_t)n;
    return 1;
}