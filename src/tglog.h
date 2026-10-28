/*
 * tglog.h — append-only debug log written straight to a file.
 * NSLog on iOS prefers the Apple System Log and can leave stderr empty when
 * the app was started from a shell, which makes field debugging blind.
 */
#ifndef TGLOG_H
#define TGLOG_H

void tglog_write(const char *fmt, ...);

/* A directory this process may actually write to (/tmp, or the sandbox
 * container's tmp when the app was started by SpringBoard). */
const char *tg_dir(void);

#endif
