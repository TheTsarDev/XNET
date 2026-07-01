/**
 * xnet_ui.h
 * pbkit rendering for XNET — all screen draw calls
 *
 * Color format: 0xAARRGGBB
 * Resolution: 640×480
 * Font: 8×8 bitmap font (built-in)
 */

#ifndef XNET_UI_H
#define XNET_UI_H

#include <stdint.h>
#include "main.h"

/* ── BUTTON CONSTANTS ────────────────────────────────────────────────────────── */
#define BTN_A       (1 << 0)
#define BTN_B       (1 << 1)
#define BTN_X       (1 << 2)
#define BTN_Y       (1 << 3)
#define BTN_START   (1 << 4)
#define BTN_BACK    (1 << 5)
#define BTN_DPAD_UP    (1 << 6)
#define BTN_DPAD_DOWN  (1 << 7)
#define BTN_DPAD_LEFT  (1 << 8)
#define BTN_DPAD_RIGHT (1 << 9)

/* ── SCREEN DIMENSIONS ───────────────────────────────────────────────────────── */
#define SCREEN_W    640
#define SCREEN_H    480

/* ── INPUT ───────────────────────────────────────────────────────────────────── */
/** Returns 1 if button was just pressed this frame (edge detect). */
int  xnet_ui_button_pressed(int btn);
int  xnet_ui_dpad_up(void);
int  xnet_ui_dpad_down(void);
int  xnet_ui_dpad_left(void);
int  xnet_ui_dpad_right(void);

/** Right/Left trigger edge-detect (analog axis crossed press threshold this
 *  frame). Used by Secure Transfer to open the browser and pick a file. */
int  xnet_ui_rtrigger_pressed(void);
int  xnet_ui_ltrigger_pressed(void);

/* ── DRAW SCREENS ────────────────────────────────────────────────────────────── */
void xnet_ui_draw_main_menu(const char** items, int count, int cursor,
                             const char* version);

void xnet_ui_draw_token_display(const char* token,
                                 const int* peers_online, int max_slots);

void xnet_ui_draw_keyboard(const char** rows, int row_count, int col_count,
                            int cur_row, int cur_col,
                            const char* input_buf,
                            const char* title, const char* prompt);

void xnet_ui_draw_chat(const ChatMessage* history, int history_count,
                       const char* input_buf,
                       const int* peers_online, uint8_t my_slot,
                       const uint32_t* slot_colors,
                       const char** kbd_rows, int kbd_row_count, int kbd_col_count,
                       int cur_row, int cur_col);

void xnet_ui_draw_error(const char* msg);

/* ── SECURE TRANSFER SCREENS ─────────────────────────────────────────────────── */
#include "xnet_files.h"

/** SEND / RECEIVE chooser. cursor: 0=SEND, 1=RECEIVE. */
void xnet_ui_draw_xfer_menu(int cursor);

/** Sender token screen. peer_connected flips the hint to "press RT to browse". */
void xnet_ui_draw_xfer_send_wait(const char* token, int peer_connected);

/** File browser. Reads list/cursor/scroll from the xnet_files module. */
void xnet_ui_draw_browser(const XFileList* fl);

/** Confirm the picked file before sending. */
void xnet_ui_draw_xfer_confirm(const char* filename, unsigned long size);

/** Receiver idle screen shown after joining, before any transfer arrives. */
void xnet_ui_draw_xfer_wait_files(void);

/** Two-bar progress (sender→relay, relay→receiver), 0..100 each. */
void xnet_ui_draw_xfer_progress(const char* title, const char* filename,
                                int up_pct, int down_pct);

/** Final result. success=1 → SUCCESS (green); else FAILED with err code. */
void xnet_ui_draw_xfer_result(int success, int err_code, const char* filename);

/** Secure Video: START / JOIN chooser. */
void xnet_ui_draw_video_menu(int cursor);

/** Secure Video: 2x2 participant tile grid. Pulls decoded frames from
    xnet_video. cam_ok = local camera streaming (controls the YOU tile label). */
void xnet_ui_draw_video_grid(int my_slot, const int* peers_online, int cam_ok,
                             int headset_ok, int talking);


/** Boot splash with status line — usable before any subsystem init. */
void xnet_ui_draw_splash(const char* status);

void xnet_ui_draw_settings(int cursor, const char* relay_ip, int relay_port,
                           uint32_t gate, uint32_t energy, int talking,
                           int debug_on, int mic_gain, uint32_t peak);
void xnet_ui_draw_about(int* scroll);

/** Set the "RELAY: host:port" line shown on the main menu. */
void xnet_ui_set_relay_info(const char* s);

/** Present rendered frame to display. Call once per loop iteration. */
void xnet_ui_flip(void);

#endif /* XNET_UI_H */

/** Chat-screen voice indicator: e.g. "MIC ON" / "NO HEADSET"; talking
 *  highlights it. Empty string hides it. */
void xnet_ui_set_voice_status(const char* s, int talking);
