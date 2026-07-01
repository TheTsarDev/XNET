/**
 * xnet_log.h
 * Persistent debug logging to E:\Dashboard\system\tsar.log
 */

#ifndef XNET_LOG_H
#define XNET_LOG_H

/** Mount E: and open the log file. Returns 0 on success, -1 on failure.
 *  Safe to call before any other subsystem. On failure, xnet_logf()
 *  becomes a silent no-op. */
int xnet_log_init(void);

/** Build date/time string, regenerated on every build by the Makefile
 *  (xnet_buildstamp.c). Replaces __DATE__/__TIME__, which only update when
 *  the translation unit holding them happens to recompile. */
const char* xnet_build_stamp(void);

/** printf-style log line, prefixed with GetTickCount() ms, flushed
 *  to disk immediately. */
void xnet_logf(const char* fmt, ...);

/* High-frequency per-frame logging (camera iso stats, video tx/rx, voice
 * stats). Gated at RUNTIME by a flag toggled from Settings ("DEBUG LOGGING"),
 * so normal use stays quiet but a tester can capture a full trace without a
 * custom build. The heartbeat is NOT gated by this. XNET_VERBOSE_DEFAULT is
 * the power-on state before config is loaded. */
#define XNET_VERBOSE_DEFAULT 0

void xnet_log_set_verbose(int on);
int  xnet_log_get_verbose(void);
void xnet_vlogf_impl(const char* fmt, ...);
#define xnet_vlogf(...) xnet_vlogf_impl(__VA_ARGS__)

#endif /* XNET_LOG_H */
