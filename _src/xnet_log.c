/**
 * xnet_log.c
 * Persistent boot/debug logging to E:\Dashboard\system\xnet.log
 *
 * Design constraints:
 *   - Must work before video, network, SDL, or anything else is up.
 *     Only depends on the kernel (drive mount + file I/O).
 *   - Every line is fflush()ed immediately, so if the app hangs or
 *     crashes, the log on disk shows exactly how far it got.
 *   - If the log can't be opened, all calls become no-ops — logging
 *     must never be able to cause its own failure.
 *   - xnet_vlogf() (verbose per-frame logging) is gated by a runtime flag
 *     toggled from Settings, so normal builds stay quiet but a tester can
 *     turn on a full trace without a custom build.
 */

#include "xnet_log.h"

#include <nxdk/mount.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_PATH "E:\\Dashboard\\system\\xnet.log"

static FILE* g_log = NULL;
static int   g_verbose = XNET_VERBOSE_DEFAULT;   /* runtime verbose gate */

int xnet_log_init(void) {
    /* E: = shell partition (Harddisk0 Partition1). Mount if not already. */
    if (!nxIsDriveMounted('E')) {
        if (!nxMountDrive('E', "\\Device\\Harddisk0\\Partition1")) {
            return -1;
        }
    }

    /* Make sure the directory chain exists (no-op if it already does). */
    CreateDirectoryA("E:\\Dashboard", NULL);
    CreateDirectoryA("E:\\Dashboard\\system", NULL);

    g_log = fopen(LOG_PATH, "w");
    if (!g_log) return -1;

    xnet_logf("==== XNET boot log ====");
    xnet_logf("build: %s", xnet_build_stamp());   /* regenerated every build — see Makefile */
    return 0;
}

void xnet_log_set_verbose(int on) { g_verbose = on ? 1 : 0; }
int  xnet_log_get_verbose(void)   { return g_verbose; }

void xnet_logf(const char* fmt, ...) {
    if (!g_log) return;

    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    fprintf(g_log, "[%8lu] %s\n", (unsigned long)GetTickCount(), line);
    fflush(g_log); /* survive a hang/crash on the very next line of code */
}

void xnet_vlogf_impl(const char* fmt, ...) {
    if (!g_log || !g_verbose) return;

    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    fprintf(g_log, "[%8lu] %s\n", (unsigned long)GetTickCount(), line);
    fflush(g_log);
}
