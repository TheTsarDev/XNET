/**
 * xnet_files.c
 * On-console file browser for XNET Secure Transfer.
 *
 * Enumeration uses the Win32 FindFirstFileA/FindNextFileA/FindClose family,
 * which current NXDK provides (the same winapi layer that backs the
 * CreateDirectoryA call in xnet_log.c). All enumeration is isolated in
 * list_dir() so it can be swapped if a given NXDK lacks it.
 */

#include "xnet_files.h"
#include "xnet_log.h"

#include <nxdk/mount.h>
#include <windows.h>
#include <string.h>
#include <stdio.h>

static XFileList g_list;

/* ── DRIVE TABLE ─────────────────────────────────────────────────────────────────
 * Standard Original Xbox partition → device mapping. D: is the launch volume
 * (DVD or the dir the XBE booted from) and is already mounted by the loader. */
typedef struct { char letter; const char* device; int mountable; } DriveDef;
static const DriveDef DRIVES[] = {
    { 'C', "\\Device\\Harddisk0\\Partition2", 1 }, /* dashboard / system   */
    { 'E', "\\Device\\Harddisk0\\Partition1", 1 }, /* main data / saves    */
    { 'F', "\\Device\\Harddisk0\\Partition6", 1 }, /* extended (if present)*/
    { 'G', "\\Device\\Harddisk0\\Partition7", 1 }, /* extended (if present)*/
    { 'X', "\\Device\\Harddisk0\\Partition3", 1 }, /* cache                */
    { 'Y', "\\Device\\Harddisk0\\Partition4", 1 }, /* cache                */
    { 'Z', "\\Device\\Harddisk0\\Partition5", 1 }, /* cache                */
    { 'D', NULL,                              0 }, /* launch volume        */
};
#define DRIVE_COUNT ((int)(sizeof(DRIVES)/sizeof(DRIVES[0])))

/* ── HELPERS ─────────────────────────────────────────────────────────────────── */
static void clamp_scroll(void) {
    if (g_list.cursor < 0) g_list.cursor = 0;
    if (g_list.cursor >= g_list.count) g_list.cursor = g_list.count ? g_list.count - 1 : 0;
    if (g_list.cursor < g_list.scroll)
        g_list.scroll = g_list.cursor;
    if (g_list.cursor >= g_list.scroll + XFILES_VISIBLE_ROWS)
        g_list.scroll = g_list.cursor - XFILES_VISIBLE_ROWS + 1;
    if (g_list.scroll < 0) g_list.scroll = 0;
}

static void add_entry(const char* name, int is_dir, unsigned long size) {
    if (g_list.count >= XFILES_MAX) return;
    XFileEntry* e = &g_list.entries[g_list.count++];
    strncpy(e->name, name, XFILES_NAME_MAX - 1);
    e->name[XFILES_NAME_MAX - 1] = 0;
    e->is_dir = is_dir;
    e->size   = size;
}

/* sort: directories first, then files; original discovery order within each.
 * tmp is static (not on the stack) so the larger XFILES_MAX can't blow the
 * console's modest stack. */
static XFileEntry g_sort_tmp[XFILES_MAX];
static void partition_dirs_first(void) {
    int n = 0;
    for (int i = 0; i < g_list.count; i++)
        if (g_list.entries[i].is_dir) g_sort_tmp[n++] = g_list.entries[i];
    for (int i = 0; i < g_list.count; i++)
        if (!g_list.entries[i].is_dir) g_sort_tmp[n++] = g_list.entries[i];
    memcpy(g_list.entries, g_sort_tmp, sizeof(XFileEntry) * g_list.count);
}

/* probe a drive letter for a readable filesystem (mount on demand) */
static int drive_available(const DriveDef* d) {
    if (d->mountable && d->device) {
        if (!nxIsDriveMounted(d->letter)) {
            if (!nxMountDrive(d->letter, d->device)) return 0;
        }
    }
    char pattern[8];
    snprintf(pattern, sizeof(pattern), "%c:\\*", d->letter);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        /* an empty-but-valid volume returns ERROR_FILE_NOT_FOUND, still usable */
        return (GetLastError() == ERROR_FILE_NOT_FOUND) ? 1 : 0;
    }
    FindClose(h);
    return 1;
}

/* enumerate g_list.path into entries[] */
static void list_dir(void) {
    g_list.count  = 0;
    g_list.cursor = 0;
    g_list.scroll = 0;
    g_list.err    = 0;

    char pattern[XFILES_PATH_MAX + 2];
    snprintf(pattern, sizeof(pattern), "%s*", g_list.path);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_NO_MORE_FILES)
            g_list.err = (int)e; /* empty dir is not an error */
        return;
    }
    do {
        const char* nm = fd.cFileName;
        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
            continue; /* skip "." and ".." — Up handles parent */
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        add_entry(nm, is_dir, (unsigned long)fd.nFileSizeLow);
    } while (FindNextFileA(h, &fd) && g_list.count < XFILES_MAX);

    /* Why did enumeration stop? ERROR_NO_MORE_FILES = clean end. Anything
     * else means the shim quit early (files would be missing for a non-cap
     * reason). Hitting the cap means the dir has more than XFILES_MAX entries. */
    DWORD stop = GetLastError();
    FindClose(h);

    if (g_list.count >= XFILES_MAX)
        xnet_logf("files: '%s' hit cap %d (dir larger than cap)", g_list.path, XFILES_MAX);
    else if (stop != ERROR_NO_MORE_FILES)
        xnet_logf("files: '%s' enum stopped early at %d, err=%lu",
                  g_list.path, g_list.count, (unsigned long)stop);
    else
        xnet_logf("files: '%s' listed %d entries", g_list.path, g_list.count);

    partition_dirs_first();
}

/* ── PUBLIC API ──────────────────────────────────────────────────────────────── */
void xnet_files_open_root(void) {
    memset(&g_list, 0, sizeof(g_list));
    g_list.path[0] = 0; /* root */
    for (int i = 0; i < DRIVE_COUNT; i++) {
        if (drive_available(&DRIVES[i])) {
            char nm[4];
            snprintf(nm, sizeof(nm), "%c:", DRIVES[i].letter);
            add_entry(nm, 1, 0); /* drives sort as dirs */
        }
    }
    if (g_list.count == 0)
        xnet_logf("xfer: no readable drives found for browser");
}

const XFileList* xnet_files_get(void) { return &g_list; }

void xnet_files_move(int delta) {
    if (g_list.count == 0) return;
    g_list.cursor = (g_list.cursor + delta + g_list.count) % g_list.count;
    clamp_scroll();
}

int xnet_files_cursor_is_file(void) {
    if (g_list.count == 0) return 0;
    if (g_list.path[0] == 0) return 0; /* drive root: entries are drives */
    return !g_list.entries[g_list.cursor].is_dir;
}

int xnet_files_enter(void) {
    if (g_list.count == 0) return 0;
    const XFileEntry* e = &g_list.entries[g_list.cursor];

    if (g_list.path[0] == 0) {
        /* at drive list → descend into "X:\" */
        snprintf(g_list.path, sizeof(g_list.path), "%c:\\", e->name[0]);
        list_dir();
        return 1;
    }
    if (e->is_dir) {
        int n = (int)strlen(g_list.path);
        snprintf(g_list.path + n, sizeof(g_list.path) - n, "%s\\", e->name);
        list_dir();
        return 1;
    }
    return 0; /* a file — caller treats as pick */
}

void xnet_files_up(void) {
    int n = (int)strlen(g_list.path);
    if (n == 0) return;          /* already at drive list */
    n--;                          /* trailing '\' */
    int i = n - 1;
    while (i >= 0 && g_list.path[i] != '\\') i--;
    if (i < 0) {                  /* was at "X:\" → back to drive list */
        xnet_files_open_root();
        return;
    }
    g_list.path[i + 1] = 0;       /* keep up to and including that '\' */
    list_dir();
}

int xnet_files_selected_path(char* out, int out_sz) {
    if (!xnet_files_cursor_is_file()) return 0;
    const XFileEntry* e = &g_list.entries[g_list.cursor];
    snprintf(out, out_sz, "%s%s", g_list.path, e->name);
    return 1;
}
