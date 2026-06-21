/**
 * xnet_files.h
 * On-console file browser for XNET Secure Transfer.
 *
 * Presents a drive-letter root (C/E/F/G/X/Y/Z plus the launch volume D),
 * mounts partitions on demand, and walks directories with the Win32
 * FindFirstFile family. State (current path, entry list, cursor, scroll)
 * lives inside the module; callers read it through xnet_files_get().
 *
 * Paths are FATX/NT style with backslashes, e.g. "E:\\saves\\". A current
 * path of "" means the synthetic drive-list root.
 */

#ifndef XNET_FILES_H
#define XNET_FILES_H

#include <stdint.h>

#define XFILES_MAX        1024  /* entries listed per directory (rest ignored) */
#define XFILES_NAME_MAX   64    /* FATX caps filenames at 42; 64 is safe        */
#define XFILES_PATH_MAX   280
#define XFILES_VISIBLE_ROWS 16  /* rows shown at once (UI list area + scroll)   */

typedef struct {
    char          name[XFILES_NAME_MAX];
    int           is_dir;   /* 1 = directory or drive, 0 = file */
    unsigned long size;     /* bytes (low dword; Xbox files are < 4GB) */
} XFileEntry;

typedef struct {
    char        path[XFILES_PATH_MAX]; /* current dir, "" = drive-list root */
    XFileEntry  entries[XFILES_MAX];
    int         count;
    int         cursor;     /* selected index */
    int         scroll;     /* first visible row */
    int         err;        /* last enumerate error (Win32 code, 0 = ok) */
} XFileList;

/** Reset to the drive-list root and enumerate available volumes. */
void xnet_files_open_root(void);

/** Read-only view of current browser state (never NULL). */
const XFileList* xnet_files_get(void);

/** Move the cursor by delta with wrap, adjusting scroll. */
void xnet_files_move(int delta);

/** 1 if the cursor currently sits on a real file (not a dir/drive). */
int xnet_files_cursor_is_file(void);

/** Descend into the cursor entry if it's a dir/drive. Returns 1 if it
 *  descended, 0 if the cursor is a file (caller should treat as "pick"). */
int xnet_files_enter(void);

/** Go up one level; from a drive root returns to the drive list. */
void xnet_files_up(void);

/** Full path of the cursor file into out (e.g. "E:\\saves\\foo.bin").
 *  Returns 1 on success, 0 if the cursor is not a file. */
int xnet_files_selected_path(char* out, int out_sz);

#endif /* XNET_FILES_H */
