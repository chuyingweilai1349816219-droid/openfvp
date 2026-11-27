/* FVP Win32 desktop shell -- logging (UTF-8 file + optional parent console).
 *
 * A GUI-subsystem executable has no console and does not inherit stdout when
 * launched from Explorer, so every diagnostic goes to a log file next to the
 * executable (or --log PATH).  --console additionally attaches to the parent
 * console when the shell was started from one, which is handy for benchmarks.
 */
#ifndef FVP_WIN32_LOG_H
#define FVP_WIN32_LOG_H

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open the log file (UTF-16 path).  Returns 1 on success, 0 on failure. */
int fvp_log_open(const wchar_t *path_utf16);

/* Try to attach to the console of the process that started us (--console). */
void fvp_log_attach_console(void);

/* Timestamped line, printf style, always terminated with '\n'.  Thread safe. */
void fvp_logf(const char *fmt, ...);

/* Write a block without a timestamp prefix (used for the final summary). */
void fvp_log_raw(const char *text);

/* Flush + close.  Safe to call when no log was opened. */
void fvp_log_close(void);

/* 底层日志文件（没开日志时是 stderr）；只给诊断导出用。 */
FILE *fvp_log_stream(void);

#ifdef __cplusplus
}
#endif

#endif /* FVP_WIN32_LOG_H */