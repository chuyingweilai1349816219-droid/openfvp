/* FVP Win32 desktop shell -- logging implementation. */
#include "win32_log.h"

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static FILE *g_file;
static FILE *g_console;
static CRITICAL_SECTION g_cs;
static int g_cs_ready;
static LARGE_INTEGER g_freq;
static LARGE_INTEGER g_start;
static int g_timer_ready;

static void ensure_cs(void)
{
    if (!g_cs_ready) {
        InitializeCriticalSection(&g_cs);
        g_cs_ready = 1;
    }
}

int fvp_log_open(const wchar_t *path_utf16)
{
    ensure_cs();
    if (!g_timer_ready) {
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_start);
        g_timer_ready = 1;
    }
    g_file = _wfopen(path_utf16, L"wb");
    return g_file != NULL;
}

void fvp_log_attach_console(void)
{
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        _wfreopen(L"CONOUT$", L"w", stdout);
        _wfreopen(L"CONOUT$", L"w", stderr);
        g_console = stdout;
    }
}

void fvp_logf(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    int n = 0;
    int m;

    ensure_cs();
    EnterCriticalSection(&g_cs);

    if (g_timer_ready) {
        LARGE_INTEGER now;
        double t;
        QueryPerformanceCounter(&now);
        t = (double)(now.QuadPart - g_start.QuadPart) / (double)g_freq.QuadPart;
        n = snprintf(buf, sizeof(buf), "[%9.3f] ", t);
        if (n < 0) {
            n = 0;
        }
        if ((size_t)n >= sizeof(buf)) {
            n = (int)sizeof(buf) - 1;
        }
    }

    va_start(ap, fmt);
    m = vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
    va_end(ap);
    if (m < 0) {
        m = 0;
    }
    if ((size_t)(n + m) >= sizeof(buf) - 1u) {
        m = (int)sizeof(buf) - 2 - n;
        if (m < 0) {
            m = 0;
        }
    }
    n += m;
    buf[n++] = '\n';
    buf[n] = '\0';

    if (g_file != NULL) {
        fwrite(buf, 1, (size_t)n, g_file);
        fflush(g_file);
    }
    if (g_console != NULL) {
        fwrite(buf, 1, (size_t)n, g_console);
        fflush(g_console);
    }

    LeaveCriticalSection(&g_cs);
}

void fvp_log_raw(const char *text)
{
    size_t len;

    ensure_cs();
    if (text == NULL) {
        return;
    }
    len = strlen(text);
    EnterCriticalSection(&g_cs);
    if (g_file != NULL) {
        fwrite(text, 1, len, g_file);
        fflush(g_file);
    }
    if (g_console != NULL) {
        fwrite(text, 1, len, g_console);
        fflush(g_console);
    }
    LeaveCriticalSection(&g_cs);
}

void fvp_log_close(void)
{
    if (!g_cs_ready) {
        return;
    }
    EnterCriticalSection(&g_cs);
    if (g_file != NULL) {
        fclose(g_file);
        g_file = NULL;
    }
    g_console = NULL;
    LeaveCriticalSection(&g_cs);
    DeleteCriticalSection(&g_cs);
    g_cs_ready = 0;
}