#ifndef QOH99_DDRAW_LOG_H
#define QOH99_DDRAW_LOG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QOH99_WLOG_OFF     0
#define QOH99_WLOG_ON      1
#define QOH99_WLOG_VERBOSE 2

extern volatile long qoh99_wlog_gate;

void qoh99_wlog_init(void);

int  qoh99_wlog_level(void);

void qoh99_wlog_write(const char *fmt, ...);

void qoh99_wlog_write_verbose(const char *fmt, ...);

void qoh99_wlog_issue(void);
void qoh99_wlog_write_issue(const char *fmt, ...);
#define WLOG_ISSUE(...) qoh99_wlog_write_issue(__VA_ARGS__)

/* Process is exiting (DLL_PROCESS_DETACH). Sets a flag ONLY: no file, lock,
   or cross-module work - every later write becomes a no-op. */
void qoh99_wlog_mark_process_exit(void);

/* Flushes and closes the log; never call this from DllMain. */
void qoh99_wlog_close(void);

static __inline int qoh99_wlog_should(void)
{
    return qoh99_wlog_gate != 0;
}

static __inline int qoh99_wlog_should_verbose(void)
{
    const long gate = qoh99_wlog_gate;
    return gate < 0 || gate >= QOH99_WLOG_VERBOSE;
}

#ifdef __cplusplus
}
#endif

#endif
