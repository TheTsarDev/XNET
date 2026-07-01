/**
 * xnet_ui.c
 * pbkit rendering + input for XNET
 *
 * All rendering is done via pbkit's software rasterizer helpers.
 * Resolution: 640×480 @ 32bpp
 *
 * Color scheme:
 *   Background:  #0A0A0F  (near-black, deep space)
 *   Panel:       #12121C  (slightly lighter)
 *   Accent:      #00FF88  (green — slot 0 / system)
 *   Text:        #E0E0E0  (off-white)
 *   Dim:         #404055  (inactive / border)
 *   Error:       #FF3030  (red)
 *
 * Font: 8x8 pixel bitmap, 128 ASCII chars
 */

#include "xnet_ui.h"
#include "main.h"
#include "xnet_log.h"
#include "xnet_video.h"

#include <hal/debug.h>
#include <hal/video.h>
#include <windows.h>
#include <SDL.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ── COLORS ──────────────────────────────────────────────────────────────────── */
#define COL_BG          0xFF0A0A0F
#define COL_PANEL       0xFF12121C
#define COL_ACCENT      0xFF00FF88
#define COL_TEXT        0xFFE0E0E0
#define COL_DIM         0xFF404055
#define COL_ERROR       0xFFFF3030
#define COL_WHITE       0xFFFFFFFF
#define COL_YELLOW      0xFFFFFF00
#define COL_CYAN        0xFF00FFFF

/* slot colors */
static const uint32_t SLOT_COL[4] = {
    0xFF00FF88, /* slot 0 — mint green  */
    0xFF00CCFF, /* slot 1 — sky blue    */
    0xFFFFCC00, /* slot 2 — amber       */
    0xFFFF44AA, /* slot 3 — pink        */
};

/* ── FONT — 8×8 bitmap, 128 chars ───────────────────────────────────────────── */
/* Minimal printable ASCII subset — rows of 8 bytes per glyph */
/* Using a compact 8x8 font. Each char = 8 bytes (one per row, MSB = leftmost pixel) */
#include "font8x8.h"  /* external header — see note below */

/*
 * NOTE: font8x8.h is a standard public-domain 8x8 bitmap font.
 * Available at: https://github.com/dhepper/font8x8
 * Place font8x8_basic[128][8] in vendor/font8x8.h
 * or use the NXDK debug font fallback below.
 */

/* ── FRAMEBUFFER HELPERS ─────────────────────────────────────────────────────── */
/*
 * Software back buffer. All drawing goes here; xnet_ui_flip() copies it to
 * the real GPU framebuffer (XVideoGetFB) once per frame on vblank.
 *
 * Statically allocated → valid from the very first draw call. (The old pbkit
 * version left g_fb NULL until the first flip, so the first frame's draw
 * calls wrote through a NULL pointer.)
 */
static uint32_t g_backbuf[SCREEN_W * SCREEN_H];

static inline void fb_pixel(int x, int y, uint32_t color) {
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    g_backbuf[y * SCREEN_W + x] = color;
}

static void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            fb_pixel(col, row, color);
}

static void fb_draw_rect_outline(int x, int y, int w, int h, uint32_t color) {
    fb_fill_rect(x,         y,         w, 1, color); /* top */
    fb_fill_rect(x,         y + h - 1, w, 1, color); /* bottom */
    fb_fill_rect(x,         y,         1, h, color); /* left */
    fb_fill_rect(x + w - 1, y,         1, h, color); /* right */
}

static void fb_draw_char(int x, int y, char c, uint32_t color) {
    if (c < 0 || c > 127) c = '?';
    const char* glyph = font8x8_basic[(int)c];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = (uint8_t)glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (1 << col)) {
                fb_pixel(x + col, y + row, color);
            }
        }
    }
}

static void fb_draw_string(int x, int y, const char* s, uint32_t color) {
    int cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += 10; s++; continue; }
        fb_draw_char(cx, y, *s, color);
        cx += 9; /* 8px char + 1px spacing */
        s++;
    }
}

/* centered string */
static void fb_draw_string_centered(int y, const char* s, uint32_t color) {
    int len = (int)strlen(s);
    int x   = (SCREEN_W - len * 9) / 2;
    fb_draw_string(x, y, s, color);
}

/* centered within an arbitrary horizontal span [x0, x0+w) */
static void fb_draw_string_centered_in(int x0, int y, int w, const char* s, uint32_t color) {
    int len = (int)strlen(s);
    int x   = x0 + (w - len * 9) / 2;
    fb_draw_string(x, y, s, color);
}

/* large 2x scaled string */
static void fb_draw_string_2x(int x, int y, const char* s, uint32_t color) {
    int cx = x;
    while (*s) {
        char c = *s;
        if (c < 0 || c > 127) c = '?';
        const char* glyph = font8x8_basic[(int)c];
        for (int row = 0; row < 8; row++) {
            uint8_t bits = (uint8_t)glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (1 << col)) {
                    fb_pixel(cx + col*2,     y + row*2,     color);
                    fb_pixel(cx + col*2 + 1, y + row*2,     color);
                    fb_pixel(cx + col*2,     y + row*2 + 1, color);
                    fb_pixel(cx + col*2 + 1, y + row*2 + 1, color);
                }
            }
        }
        cx += 18;
        s++;
    }
}

static void fb_draw_string_2x_centered(int y, const char* s, uint32_t color) {
    int len = (int)strlen(s);
    int x   = (SCREEN_W - len * 18) / 2;
    fb_draw_string_2x(x, y, s, color);
}

/* horizontal divider line */
static void fb_hline(int x, int y, int w, uint32_t color) {
    fb_fill_rect(x, y, w, 1, color);
}

/* ── INPUT STATE (SDL2 GameController) ──────────────────────────────────────── */
static SDL_GameController* g_controller = NULL;

/* current and previous button state — one bit per SDL_GameControllerButton */
static Uint8 g_btn_cur[SDL_CONTROLLER_BUTTON_MAX];
static Uint8 g_btn_prev[SDL_CONTROLLER_BUTTON_MAX];

/* analog triggers are axes, not buttons — track a digital "pressed" view */
#define TRIGGER_PRESS_THRESHOLD 12000 /* of 32767; ~37% pull */
static Uint8 g_trig_cur[2];   /* [0]=left, [1]=right */
static Uint8 g_trig_prev[2];

static void input_poll(void) {
    static int sdl_ready = 0;
    if (!sdl_ready) {
        xnet_logf("SDL_Init(GAMECONTROLLER) starting (first input poll)...");
        int rc = SDL_Init(SDL_INIT_GAMECONTROLLER);
        xnet_logf("SDL_Init done -> %d%s%s", rc,
                  rc != 0 ? " err=" : "",
                  rc != 0 ? SDL_GetError() : "");
        sdl_ready = 1;
    }

    SDL_GameControllerUpdate();
    SDL_PumpEvents();

    /* open controller on first poll if not already open */
    if (!g_controller) {
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (SDL_IsGameController(i)) {
                g_controller = SDL_GameControllerOpen(i);
                xnet_logf("controller opened on joystick %d -> %p",
                          i, (void*)g_controller);
                break;
            }
        }
    }

    memcpy(g_btn_prev, g_btn_cur, sizeof(g_btn_cur));
    memcpy(g_trig_prev, g_trig_cur, sizeof(g_trig_cur));

    if (g_controller) {
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
            g_btn_cur[b] = SDL_GameControllerGetButton(
                g_controller, (SDL_GameControllerButton)b);
        }
        Sint16 lt = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        Sint16 rt = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        g_trig_cur[0] = (lt > TRIGGER_PRESS_THRESHOLD) ? 1 : 0;
        g_trig_cur[1] = (rt > TRIGGER_PRESS_THRESHOLD) ? 1 : 0;
    } else {
        memset(g_btn_cur, 0, sizeof(g_btn_cur));
        memset(g_trig_cur, 0, sizeof(g_trig_cur));
    }
}

/* map BTN_* constants to SDL_GameControllerButton */
static SDL_GameControllerButton btn_to_sdl(int btn) {
    switch (btn) {
        case BTN_A:          return SDL_CONTROLLER_BUTTON_A;
        case BTN_B:          return SDL_CONTROLLER_BUTTON_B;
        case BTN_X:          return SDL_CONTROLLER_BUTTON_X;
        case BTN_Y:          return SDL_CONTROLLER_BUTTON_Y;
        case BTN_START:      return SDL_CONTROLLER_BUTTON_START;
        case BTN_BACK:       return SDL_CONTROLLER_BUTTON_BACK;
        case BTN_DPAD_UP:    return SDL_CONTROLLER_BUTTON_DPAD_UP;
        case BTN_DPAD_DOWN:  return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
        case BTN_DPAD_LEFT:  return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
        case BTN_DPAD_RIGHT: return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
        default:             return SDL_CONTROLLER_BUTTON_INVALID;
    }
}

/* edge-detect: pressed this frame but not last */
int xnet_ui_button_pressed(int btn) {
    SDL_GameControllerButton sdl_btn = btn_to_sdl(btn);
    if (sdl_btn == SDL_CONTROLLER_BUTTON_INVALID) return 0;
    return g_btn_cur[sdl_btn] && !g_btn_prev[sdl_btn];
}

int xnet_ui_dpad_up(void)    { return xnet_ui_button_pressed(BTN_DPAD_UP);    }
int xnet_ui_dpad_down(void)  { return xnet_ui_button_pressed(BTN_DPAD_DOWN);  }
int xnet_ui_dpad_left(void)  { return xnet_ui_button_pressed(BTN_DPAD_LEFT);  }
int xnet_ui_dpad_right(void) { return xnet_ui_button_pressed(BTN_DPAD_RIGHT); }

int xnet_ui_ltrigger_pressed(void) { return g_trig_cur[0] && !g_trig_prev[0]; }
int xnet_ui_rtrigger_pressed(void) { return g_trig_cur[1] && !g_trig_prev[1]; }

/* ── FRAME INIT / FLIP ───────────────────────────────────────────────────────── */
void xnet_ui_flip(void) {
    static int flip_count = 0;

    /*
     * Present FIRST, poll input AFTER. input_poll() initializes SDL (and
     * with it the USB host stack) on its first call — if that ever hangs,
     * the frame we just drew is already on screen instead of black, and
     * the log shows exactly where we stopped.
     */
    XVideoWaitForVBlank();

    uint32_t* fb = (uint32_t*)XVideoGetFB();
    if (fb) {
        memcpy(fb, g_backbuf, sizeof(g_backbuf));
        XVideoFlushFB(); /* flush write-combined framebuffer memory */
    }

    /* clear back buffer to background for next frame */
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        g_backbuf[i] = COL_BG;
    }

    if (flip_count < 3) {
        xnet_logf("flip #%d presented (FB=%p)", flip_count, (void*)fb);
    }
    flip_count++;

    input_poll();
}

/* ── DEV WATERMARK (for shared screenshots) ──────────────────────────────────── */
 #define XNET_WATERMARK "PUBLIC BETA"

/* ── RELAY INFO LINE (set once at boot from config) ─────────────────────────── */
static char g_relay_info[96] = "";

static char g_voice_status[16] = "";
static int  g_voice_talking = 0;

void xnet_ui_set_voice_status(const char* s, int talking) {
    strncpy(g_voice_status, s, sizeof(g_voice_status) - 1);
    g_voice_status[sizeof(g_voice_status) - 1] = 0;
    g_voice_talking = talking;
}

void xnet_ui_set_relay_info(const char* s) {
    strncpy(g_relay_info, s, sizeof(g_relay_info) - 1);
    g_relay_info[sizeof(g_relay_info) - 1] = 0;
}

/* ── BOOT SPLASH (drawn before network/audio init so screen is never blank) ──── */
void xnet_ui_draw_splash(const char* status) {
    fb_draw_string_2x_centered(180, "XNET", COL_ACCENT);
    fb_draw_string_centered(220, "ENCRYPTED XBOX CHAT", COL_DIM);
    if (status && status[0]) {
        fb_draw_string_centered(270, status, COL_TEXT);
    }
    fb_draw_string_centered(SCREEN_H - 40, XNET_WATERMARK, COL_DIM);
}

/* ── SHARED HEADER ───────────────────────────────────────────────────────────── */
static void draw_header(const char* title) {
    /* top bar */
    fb_fill_rect(0, 0, SCREEN_W, 28, COL_PANEL);
    fb_hline(0, 28, SCREEN_W, COL_ACCENT);

    /* XNET brand left */
    fb_draw_string(10, 10, "XNET", COL_ACCENT);

    /* live RAM meter, centered — used/total in MB (0.1MB resolution) so the
       cost of entering video chat etc. is visible at a glance */
    {
        unsigned long free_kb = 0, total_mb = 0;
        xnet_mem_query(&free_kb, &total_mb);
        unsigned long used_kb = (total_mb * 1024 > free_kb) ? (total_mb * 1024 - free_kb) : 0;
        char ram[32];
        snprintf(ram, sizeof(ram), "RAM %lu.%lu/%luMB",
                 used_kb / 1024, ((used_kb % 1024) * 10) / 1024, total_mb);
        int rlen = (int)strlen(ram);
        fb_draw_string((SCREEN_W - rlen * 9) / 2, 10, ram, COL_DIM);
    }

    /* screen title right */
    int len = (int)strlen(title);
    fb_draw_string(SCREEN_W - len * 9 - 10, 10, title, COL_DIM);
}

/* ── SHARED FOOTER ───────────────────────────────────────────────────────────── */
static void draw_footer(const char* hints) {
    fb_hline(0, SCREEN_H - 22, SCREEN_W, COL_DIM);
    fb_fill_rect(0, SCREEN_H - 21, SCREEN_W, 21, COL_PANEL);
    fb_draw_string_centered(SCREEN_H - 14, hints, COL_DIM);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * SCREEN: MAIN MENU
 * ═══════════════════════════════════════════════════════════════════════════════ */
void xnet_ui_draw_main_menu(const char** items, int count, int cursor,
                             const char* version) {
    draw_header("MAIN MENU");

    /* big centered title */
    fb_draw_string_2x_centered(80, "XNET", COL_ACCENT);
    fb_draw_string_centered(118, "ENCRYPTED XBOX CHAT", COL_DIM);

    /* menu items */
    int menu_y = 170;
    int item_h = 36;

    for (int i = 0; i < count; i++) {
        int y   = menu_y + i * item_h;
        int sel = (i == cursor);

        if (sel) {
            /* highlight bar */
            fb_fill_rect(180, y - 6, 280, 26, COL_PANEL);
            fb_draw_rect_outline(180, y - 6, 280, 26, COL_ACCENT);
            fb_draw_string(200, y, items[i], COL_ACCENT);
            /* cursor arrow */
            fb_draw_string(185, y, ">", COL_ACCENT);
        } else {
            fb_draw_string(200, y, items[i], COL_TEXT);
        }
    }

    /* version bottom-left in footer */
    draw_footer("DPAD UP/DOWN  A SELECT  B BACK");

    char ver_buf[32];
    snprintf(ver_buf, sizeof(ver_buf), "v%s", version);
    fb_draw_string(10, SCREEN_H - 14, ver_buf, COL_DIM);

    /* relay target bottom-left + dev watermark right, just above the
       footer bar so neither collides with the centered button hints */
    if (g_relay_info[0]) {
        fb_draw_string(10, SCREEN_H - 36, g_relay_info, COL_DIM);
    }
    /* headset status line above the relay info (set per-frame by main loop) */
    if (g_voice_status[0]) {
        fb_draw_string(10, SCREEN_H - 58, g_voice_status,
                       g_voice_talking ? COL_ACCENT : COL_DIM);
    }
    fb_draw_string(SCREEN_W - (int)strlen(XNET_WATERMARK) * 9 - 10,
                   SCREEN_H - 36, XNET_WATERMARK, COL_DIM);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * SCREEN: TOKEN DISPLAY
 * ═══════════════════════════════════════════════════════════════════════════════ */
void xnet_ui_draw_token_display(const char* token,
                                 const int* peers_online, int max_slots) {
    draw_header("CREATE ROOM");

    fb_draw_string_centered(60, "SHARE THIS TOKEN", COL_DIM);

    /* big token display — 2x scale with spaced chars */
    /* draw each char individually with extra spacing for readability */
    int tok_len = (int)strlen(token);
    int char_w  = 22; /* 18px char + 4px gap */
    int total_w = tok_len * char_w;
    int tok_x   = (SCREEN_W - total_w) / 2;
    int tok_y   = 100;

    /* token background panel */
    fb_fill_rect(tok_x - 16, tok_y - 12, total_w + 32, 40, COL_PANEL);
    fb_draw_rect_outline(tok_x - 16, tok_y - 12, total_w + 32, 40, COL_ACCENT);

    for (int i = 0; i < tok_len; i++) {
        char ch[2] = { token[i], 0 };
        fb_draw_string_2x(tok_x + i * char_w, tok_y, ch, COL_ACCENT);
    }

    /* waiting indicator */
    fb_draw_string_centered(165, "WAITING FOR PEERS TO JOIN...", COL_DIM);

    /* slot status */
    int slot_x = (SCREEN_W - (max_slots * 70)) / 2;
    int slot_y = 210;

    fb_draw_string_centered(slot_y - 20, "SLOTS", COL_DIM);

    for (int i = 0; i < max_slots; i++) {
        int x = slot_x + i * 70;
        uint32_t col = peers_online[i] ? SLOT_COL[i] : COL_DIM;

        fb_draw_rect_outline(x, slot_y, 56, 30, col);
        if (peers_online[i]) {
            fb_fill_rect(x + 1, slot_y + 1, 54, 28, COL_PANEL);
        }

        char label[8];
        snprintf(label, sizeof(label), "P%d", i + 1);
        fb_draw_string(x + 20, slot_y + 11, label, col);
    }

    /* encryption notice */
    fb_draw_string_centered(290, "SESSION IS END-TO-END ENCRYPTED", COL_DIM);
    fb_draw_string_centered(305, "RELAY CANNOT READ YOUR MESSAGES", COL_DIM);

    draw_footer("B CANCEL");
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * SCREEN: ON-SCREEN KEYBOARD (token entry + chat input)
 * Shared keyboard renderer used by both screens
 * ═══════════════════════════════════════════════════════════════════════════════ */

#define KBD_KEY_W   50
#define KBD_KEY_H   28
#define KBD_PAD_X   4
#define KBD_PAD_Y   4

static void draw_keyboard_at(int kbd_x, int kbd_y,
                              const char** rows, int row_count, int col_count,
                              int cur_row, int cur_col) {
    for (int r = 0; r < row_count; r++) {
        for (int c = 0; c < col_count; c++) {
            int     x   = kbd_x + c * (KBD_KEY_W + KBD_PAD_X);
            int     y   = kbd_y + r * (KBD_KEY_H + KBD_PAD_Y);
            int     sel = (r == cur_row && c == cur_col);
            char    key = rows[r][c];

            uint32_t bg_col  = sel ? COL_ACCENT : COL_PANEL;
            uint32_t txt_col = sel ? COL_BG     : COL_TEXT;
            uint32_t brd_col = sel ? COL_ACCENT : COL_DIM;

            /* special keys */
            uint32_t label_col = txt_col;
            if (!sel) {
                if (key == '<') label_col = COL_ERROR;
                if (key == '-') label_col = COL_ACCENT;
            }

            fb_fill_rect(x, y, KBD_KEY_W, KBD_KEY_H, bg_col);
            fb_draw_rect_outline(x, y, KBD_KEY_W, KBD_KEY_H, brd_col);

            /* key label */
            char lbl[3] = { key, 0, 0 };
            if (key == '<') { lbl[0] = '<'; lbl[1] = '-'; }
            if (key == '-') { lbl[0] = 'O'; lbl[1] = 'K'; }
            if (key == ' ') { lbl[0] = '_'; lbl[1] = 0; }

            int lbl_len = (int)strlen(lbl);
            int lbl_x   = x + (KBD_KEY_W - lbl_len * 9) / 2;
            int lbl_y   = y + (KBD_KEY_H - 8) / 2;
            fb_draw_string(lbl_x, lbl_y, lbl, label_col);
        }
    }
}

void xnet_ui_draw_keyboard(const char** rows, int row_count, int col_count,
                            int cur_row, int cur_col,
                            const char* input_buf,
                            const char* title, const char* prompt) {
    draw_header(title);

    fb_draw_string_centered(40, prompt, COL_DIM);

    /* input display */
    int inp_x   = (SCREEN_W - 200) / 2;
    int inp_y   = 60;
    fb_fill_rect(inp_x, inp_y, 200, 28, COL_PANEL);
    fb_draw_rect_outline(inp_x, inp_y, 200, 28, COL_ACCENT);

    int buf_len = (int)strlen(input_buf);
    if (buf_len <= 8) {
        /* short input (tokens) — big 2x chars with spacing */
        int ch_x = inp_x + 8;
        for (int i = 0; i < buf_len; i++) {
            char ch[2] = { input_buf[i], 0 };
            fb_draw_string_2x(ch_x + i * 22, inp_y + 5, ch, COL_ACCENT);
        }
        if (buf_len < 8)
            fb_draw_string_2x(ch_x + buf_len * 22, inp_y + 5, "_", COL_DIM);
    } else {
        /* long input (relay IP) — 1x font so it fits the box */
        fb_draw_string(inp_x + 8, inp_y + 10, input_buf, COL_ACCENT);
    }

    /* keyboard */
    int kbd_total_w = col_count * (KBD_KEY_W + KBD_PAD_X) - KBD_PAD_X;
    int kbd_x       = (SCREEN_W - kbd_total_w) / 2;
    int kbd_y       = 115;
    draw_keyboard_at(kbd_x, kbd_y, rows, row_count, col_count, cur_row, cur_col);

    draw_footer("DPAD NAVIGATE  A SELECT  B BACK  OK CONFIRM");
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * SCREEN: CHAT ROOM
 * Layout:
 *   [0–27]   header bar
 *   [28–38]  peer status strip
 *   [39–270] chat history (scrolling)
 *   [271–275] divider
 *   [276–460] keyboard
 *   [461–479] footer
 * ═══════════════════════════════════════════════════════════════════════════════ */
void xnet_ui_draw_chat(const ChatMessage* history, int history_count,
                       const char* input_buf,
                       const int* peers_online, uint8_t my_slot,
                       const uint32_t* slot_colors,
                       const char** kbd_rows, int kbd_row_count, int kbd_col_count,
                       int cur_row, int cur_col) {
    draw_header("XNET CHAT");

    /* ── peer status strip ── */
    int strip_y = 32;
    fb_fill_rect(0, strip_y, SCREEN_W, 22, COL_PANEL);

    for (int i = 0; i < 4; i++) {
        int x   = 10 + i * 80;
        int on  = peers_online[i];
        uint32_t col = on ? SLOT_COL[i] : COL_DIM;

        /* dot indicator */
        fb_fill_rect(x, strip_y + 7, 8, 8, on ? col : COL_DIM);

        char label[16];
        if (i == my_slot) snprintf(label, sizeof(label), "P%d (YOU)", i + 1);
        else              snprintf(label, sizeof(label), "P%d", i + 1);

        fb_draw_string(x + 12, strip_y + 7, label, col);
    }

    /* far right: voice + encryption status */
    if (g_voice_status[0]) {
        fb_draw_string(SCREEN_W - 225, strip_y + 7, g_voice_status,
                       g_voice_talking ? COL_ACCENT : COL_DIM);
    }
    fb_draw_string(SCREEN_W - 108, strip_y + 7, "[ENCRYPTED]", COL_ACCENT);

    fb_hline(0, strip_y + 22, SCREEN_W, COL_DIM);

    /* ── chat history ── */
    int chat_area_y = 58;
    int chat_area_h = 200;
    int line_h      = 12;
    int visible_lines = chat_area_h / line_h;

    /* show last N messages */
    int start = history_count - visible_lines;
    if (start < 0) start = 0;

    for (int i = start; i < history_count; i++) {
        const ChatMessage* m = &history[i];
        int   line_y  = chat_area_y + (i - start) * line_h;
        uint32_t col  = SLOT_COL[m->slot % 4];

        char prefix[8];
        snprintf(prefix, sizeof(prefix), "P%d: ", m->slot + 1);

        if (m->is_local) {
            /* right-align our own messages */
            int msg_len  = (int)strlen(m->text);
            int pre_len  = (int)strlen(prefix);
            int total_w  = (pre_len + msg_len) * 9;
            int rx       = SCREEN_W - total_w - 10;
            fb_draw_string(rx, line_y, prefix, col);
            fb_draw_string(rx + pre_len * 9, line_y, m->text, COL_TEXT);
        } else {
            fb_draw_string(10, line_y, prefix, col);
            fb_draw_string(10 + (int)strlen(prefix) * 9, line_y, m->text, COL_TEXT);
        }
    }

    /* ── input area label ── */
    fb_hline(0, chat_area_y + chat_area_h, SCREEN_W, COL_DIM);
    int input_y = chat_area_y + chat_area_h + 4;

    /* show current input buffer */
    fb_draw_string(10, input_y, ">", COL_ACCENT);
    fb_draw_string(22, input_y, input_buf, COL_TEXT);

    /* cursor */
    int cur_x = 22 + (int)strlen(input_buf) * 9;
    if (cur_x < SCREEN_W - 10) {
        fb_draw_string(cur_x, input_y, "_", COL_DIM);
    }

    fb_hline(0, input_y + 14, SCREEN_W, COL_DIM);

    /* ── keyboard (compact, bottom half) ── */
    int kbd_total_w = kbd_col_count * (KBD_KEY_W + KBD_PAD_X) - KBD_PAD_X;
    int kbd_x       = (SCREEN_W - kbd_total_w) / 2;
    int kbd_y       = input_y + 18;
    draw_keyboard_at(kbd_x, kbd_y, kbd_rows, kbd_row_count, kbd_col_count,
                     cur_row, cur_col);

    draw_footer("DPAD NAVIGATE  A SELECT  OK SEND  START LEAVE");
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * SCREEN: ERROR
 * ═══════════════════════════════════════════════════════════════════════════════ */
void xnet_ui_draw_error(const char* msg) {
    draw_header("ERROR");

    /* error icon */
    fb_draw_string_2x_centered(130, "!", COL_ERROR);
    fb_draw_string_centered(180, msg, COL_ERROR);
    fb_draw_string_centered(210, "PRESS B TO RETURN", COL_DIM);

    draw_footer("B BACK TO MAIN MENU");
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * SECURE TRANSFER SCREENS
 * ═══════════════════════════════════════════════════════════════════════════════ */

/* human-readable byte size into buf */
static void fmt_size(unsigned long bytes, char* buf, int buf_sz) {
    if (bytes >= 1024UL * 1024UL)
        snprintf(buf, buf_sz, "%lu.%lu MB", bytes / (1024*1024),
                 ((bytes % (1024*1024)) * 10) / (1024*1024));
    else if (bytes >= 1024UL)
        snprintf(buf, buf_sz, "%lu.%lu KB", bytes / 1024, ((bytes % 1024) * 10) / 1024);
    else
        snprintf(buf, buf_sz, "%lu B", bytes);
}

/* progress bar: outline + fill proportional to pct (0..100) */
static void fb_draw_bar(int x, int y, int w, int h, int pct, uint32_t fill) {
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    fb_fill_rect(x, y, w, h, COL_PANEL);
    fb_draw_rect_outline(x, y, w, h, COL_DIM);
    int fw = ((w - 4) * pct) / 100;
    if (fw > 0) fb_fill_rect(x + 2, y + 2, fw, h - 4, fill);
}

void xnet_ui_draw_xfer_menu(int cursor) {
    draw_header("SECURE TRANSFER");

    fb_draw_string_2x_centered(80, "SECURE TRANSFER", COL_ACCENT);
    fb_draw_string_centered(118, "ENCRYPTED 1-TO-1 FILE TRANSFER", COL_DIM);

    const char* items[2] = { "SEND", "RECEIVE" };
    int menu_y = 180, item_h = 40;
    for (int i = 0; i < 2; i++) {
        int y   = menu_y + i * item_h;
        int sel = (i == cursor);
        if (sel) {
            fb_fill_rect(200, y - 6, 240, 28, COL_PANEL);
            fb_draw_rect_outline(200, y - 6, 240, 28, COL_ACCENT);
            fb_draw_string(225, y, ">", COL_ACCENT);
            fb_draw_string(245, y, items[i], COL_ACCENT);
        } else {
            fb_draw_string(245, y, items[i], COL_TEXT);
        }
    }

    draw_footer("DPAD UP/DOWN  A SELECT  B BACK");
}

void xnet_ui_draw_xfer_send_wait(const char* token, int peer_connected) {
    draw_header("SEND FILE");

    fb_draw_string_centered(50, "SHARE THIS TOKEN WITH THE RECEIVER", COL_DIM);

    int tok_len = (int)strlen(token);
    int char_w  = 22;
    int total_w = tok_len * char_w;
    int tok_x   = (SCREEN_W - total_w) / 2;
    int tok_y   = 92;
    fb_fill_rect(tok_x - 16, tok_y - 12, total_w + 32, 40, COL_PANEL);
    fb_draw_rect_outline(tok_x - 16, tok_y - 12, total_w + 32, 40, COL_ACCENT);
    for (int i = 0; i < tok_len; i++) {
        char ch[2] = { token[i], 0 };
        fb_draw_string_2x(tok_x + i * char_w, tok_y, ch, COL_ACCENT);
    }

    if (peer_connected) {
        fb_draw_string_centered(170, "RECEIVER CONNECTED", SLOT_COL[1]);
        fb_fill_rect(170, 200, 300, 30, COL_PANEL);
        fb_draw_rect_outline(170, 200, 300, 30, COL_ACCENT);
        fb_draw_string_centered(208, "PRESS RT TO BROWSE FILES", COL_ACCENT);
    } else {
        fb_draw_string_centered(185, "WAITING FOR RECEIVER TO JOIN...", COL_DIM);
    }

    fb_draw_string_centered(300, "TRANSFER IS END-TO-END ENCRYPTED", COL_DIM);
    draw_footer(peer_connected ? "RT BROWSE FILES  B CANCEL" : "B CANCEL");
}

void xnet_ui_draw_browser(const XFileList* fl) {
    draw_header("CHOOSE FILE");

    /* path bar */
    fb_fill_rect(0, 30, SCREEN_W, 18, COL_PANEL);
    const char* p = (fl->path[0]) ? fl->path : "(SELECT A DRIVE)";
    fb_draw_string(10, 35, p, COL_ACCENT);

    int list_x = 10, list_y = 56, row_h = 12;
    int rows   = XFILES_VISIBLE_ROWS;

    if (fl->count == 0) {
        fb_draw_string_centered(140, fl->err ? "CANNOT READ THIS LOCATION"
                                              : "EMPTY", COL_DIM);
    }

    for (int r = 0; r < rows; r++) {
        int idx = fl->scroll + r;
        if (idx >= fl->count) break;
        const XFileEntry* e = &fl->entries[idx];
        int y   = list_y + r * row_h;
        int sel = (idx == fl->cursor);

        if (sel) {
            fb_fill_rect(list_x - 4, y - 1, SCREEN_W - 12, row_h, COL_PANEL);
            fb_draw_string(list_x - 2, y, ">", COL_ACCENT);
        }
        uint32_t col = sel ? COL_ACCENT : (e->is_dir ? SLOT_COL[1] : COL_TEXT);

        char line[80];
        if (e->is_dir) snprintf(line, sizeof(line), "[%s]", e->name);
        else           snprintf(line, sizeof(line), "%s", e->name);
        fb_draw_string(list_x + 12, y, line, col);

        if (!e->is_dir) {
            char sz[16];
            fmt_size(e->size, sz, sizeof(sz));
            fb_draw_string(SCREEN_W - 110, y, sz, sel ? COL_ACCENT : COL_DIM);
        }
    }

    /* scroll indicator */
    if (fl->count > rows) {
        char pos[24];
        snprintf(pos, sizeof(pos), "%d/%d", fl->cursor + 1, fl->count);
        fb_draw_string(SCREEN_W - 90, 35, pos, COL_DIM);
    }

    draw_footer("DPAD MOVE  A OPEN  RT SEND FILE  B BACK");
}

void xnet_ui_draw_xfer_confirm(const char* filename, unsigned long size) {
    draw_header("CONFIRM");

    fb_draw_string_centered(110, "SEND THIS FILE?", COL_TEXT);

    fb_fill_rect(90, 150, SCREEN_W - 180, 60, COL_PANEL);
    fb_draw_rect_outline(90, 150, SCREEN_W - 180, 60, COL_ACCENT);
    fb_draw_string_centered(165, filename, COL_ACCENT);
    char sz[16];
    fmt_size(size, sz, sizeof(sz));
    fb_draw_string_centered(185, sz, COL_DIM);

    fb_draw_string_centered(250, "A  CONFIRM AND SEND", SLOT_COL[1]);
    fb_draw_string_centered(270, "B  CHOOSE ANOTHER", COL_DIM);

    draw_footer("A SEND  B BACK");
}

void xnet_ui_draw_xfer_wait_files(void) {
    draw_header("RECEIVE FILE");

    fb_draw_string_2x_centered(150, "WAITING FOR FILES", COL_ACCENT);
    fb_draw_string_centered(200, "CONNECTED - READY TO RECEIVE", COL_DIM);
    fb_draw_string_centered(230, "INCOMING FILES SAVE TO  E:\\XNET FILES", COL_DIM);

    draw_footer("B LEAVE");
}

void xnet_ui_draw_xfer_progress(const char* title, const char* filename,
                                int up_pct, int down_pct) {
    draw_header(title);

    fb_draw_string_centered(60, filename, COL_TEXT);

    int bx = 90, bw = SCREEN_W - 180, bh = 26;

    fb_draw_string(bx, 120, "SENDER -> RELAY", COL_DIM);
    fb_draw_bar(bx, 134, bw, bh, up_pct, SLOT_COL[0]);
    {
        char pc[8]; snprintf(pc, sizeof(pc), "%d%%", up_pct);
        fb_draw_string(bx + bw - 40, 138, pc, COL_BG);
    }

    fb_draw_string(bx, 190, "RELAY -> RECEIVER", COL_DIM);
    fb_draw_bar(bx, 204, bw, bh, down_pct, SLOT_COL[1]);
    {
        char pc[8]; snprintf(pc, sizeof(pc), "%d%%", down_pct);
        fb_draw_string(bx + bw - 40, 208, pc, COL_BG);
    }

    fb_draw_string_centered(270, "DO NOT POWER OFF DURING TRANSFER", COL_DIM);
    draw_footer("START ABORT");
}

void xnet_ui_draw_xfer_result(int success, int err_code, const char* filename) {
    draw_header("TRANSFER COMPLETE");

    if (success) {
        fb_draw_string_2x_centered(140, "SUCCESS", COL_ACCENT);
        fb_draw_string_centered(185, filename, COL_TEXT);
    } else {
        fb_draw_string_2x_centered(140, "FAILED", COL_ERROR);
        char code[32];
        snprintf(code, sizeof(code), "ERROR CODE  E%02d", err_code);
        fb_draw_string_centered(185, code, COL_ERROR);
    }

    fb_draw_string_centered(240, "PRESS B TO RETURN TO MENU", COL_DIM);
    draw_footer("B BACK TO MAIN MENU");
}

/* ── SECURE VIDEO ─────────────────────────────────────────────────────────────── */

/* nearest-neighbour scale-blit of an ARGB source into a back-buffer rect */
static void fb_blit_scaled(int dx, int dy, int dw, int dh,
                           const uint32_t* src, int sw, int sh) {
    if (!src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int y = 0; y < dh; y++) {
        int sy = (y * sh) / dh;
        if (sy >= sh) sy = sh - 1;
        const uint32_t* srow = src + sy * sw;
        int py = dy + y;
        if (py < 0 || py >= SCREEN_H) continue;
        uint32_t* drow = g_backbuf + py * SCREEN_W;
        for (int x = 0; x < dw; x++) {
            int sx = (x * sw) / dw;
            if (sx >= sw) sx = sw - 1;
            int px = dx + x;
            if (px < 0 || px >= SCREEN_W) continue;
            drow[px] = srow[sx];
        }
    }
}

void xnet_ui_draw_video_menu(int cursor) {
    draw_header("SECURE VIDEO");
    fb_draw_string_2x_centered(80, "SECURE VIDEO", COL_ACCENT);
    fb_draw_string_centered(118, "ENCRYPTED CAMERA CHAT", COL_DIM);

    const char* items[2] = { "START SESSION", "JOIN SESSION" };
    int menu_y = 180, item_h = 40;
    for (int i = 0; i < 2; i++) {
        int y   = menu_y + i * item_h;
        int sel = (i == cursor);
        if (sel) {
            fb_fill_rect(190, y - 6, 260, 28, COL_PANEL);
            fb_draw_rect_outline(190, y - 6, 260, 28, COL_ACCENT);
            fb_draw_string(212, y, ">", COL_ACCENT);
            fb_draw_string(232, y, items[i], COL_ACCENT);
        } else {
            fb_draw_string(232, y, items[i], COL_TEXT);
        }
    }
    draw_footer("DPAD UP/DOWN  A SELECT  B BACK");
}

/* The 2x2 tile grid. Pulls decoded frames straight from xnet_video. */
void xnet_ui_draw_video_grid(int my_slot, const int* peers_online, int cam_ok,
                             int headset_ok, int talking) {
    draw_header("SECURE VIDEO");

    /* cell geometry under the header bar */
    const int gx = 8, gy = 36, gap = 8;
    const int cw = (SCREEN_W - 2 * gx - gap) / 2;   /* ~308 */
    const int ch = (SCREEN_H - gy - 26 - gap) / 2;  /* ~204 */
    const int lab_h = 16;

    for (int slot = 0; slot < 4; slot++) {
        int col = slot & 1;
        int rowi = slot >> 1;
        int cx = gx + col * (cw + gap);
        int cy = gy + rowi * (ch + gap);
        int iw = cw, ih = ch - lab_h;

        uint32_t border = (slot < 4) ? SLOT_COL[slot] : COL_DIM;
        int present = (slot == my_slot) || (peers_online && peers_online[slot]);

        if (!present) {
            fb_draw_rect_outline(cx, cy, cw, ch, COL_DIM);
            fb_draw_string_centered_in(cx, cy + ih / 2 - 4, cw, "-", COL_DIM);
            continue;
        }

        /* image area */
        if (xnet_video_slot_has(slot)) {
            fb_blit_scaled(cx + 1, cy + 1, iw - 2, ih - 2,
                           xnet_video_slot_pixels(slot), XVID_DW, XVID_DH);
        } else {
            fb_fill_rect(cx + 1, cy + 1, iw - 2, ih - 2, COL_PANEL);
            const char* msg = "CONNECTING";
            if (slot == my_slot && !cam_ok) msg = "NO CAMERA";
            else if (xnet_video_slot_camoff(slot)) msg = "CAMERA OFF";
            fb_draw_string_centered_in(cx, cy + ih / 2 - 4, cw, msg, COL_DIM);
        }
        fb_draw_rect_outline(cx, cy, cw, ch, border);

        /* label strip — the local tile also shows live mic/talk state so audio
           can be tested at a glance (mirrors the chat screen indicator) */
        char label[28];
        uint32_t labcol = border;
        if (slot == my_slot) {
            const char* mic = !headset_ok ? "NO MIC" : (talking ? "TALKING" : "MIC ON");
            snprintf(label, sizeof(label), "YOU - %s", mic);
            if (talking)          labcol = COL_ACCENT;   /* green while speaking */
            else if (!headset_ok) labcol = COL_ERROR;    /* red if no headset    */
        } else {
            snprintf(label, sizeof(label), "PLAYER %d", slot + 1);
        }
        fb_fill_rect(cx + 1, cy + ch - lab_h, cw - 2, lab_h - 1, COL_PANEL);
        fb_draw_string(cx + 6, cy + ch - lab_h + 4, label, labcol);
    }

    /* ── on-screen debug strip (replaces logs we can't pull off hardware) ──
       For each slot: P=peers_online H=s_has  rx=stashed ok=decoded rc=pjpeg */
    {
        char dbg[64];
        int dy = SCREEN_H - 24;
        snprintf(dbg, sizeof(dbg), "me=%d peers=%d%d%d%d", my_slot,
                 peers_online ? peers_online[0] : 0, peers_online ? peers_online[1] : 0,
                 peers_online ? peers_online[2] : 0, peers_online ? peers_online[3] : 0);
        fb_draw_string(8, dy, dbg, COL_DIM);
        for (int s = 0; s < MAX_SLOTS; s++) {
            int rx = 0, ok = 0, rc = -9, pend = 0;
            xnet_video_slot_stats(s, &rx, &ok, &rc, &pend);
            if (rx == 0 && s != my_slot) continue;   /* only show active slots */
            char l[40];
            snprintf(l, sizeof(l), "s%d H%d rx%d ok%d rc%d", s,
                     xnet_video_slot_has(s), rx, ok, rc);
            fb_draw_string(160 + s * 150, dy, l, COL_DIM);
        }
    }

    draw_footer("Y CAMERA ON/OFF   START LEAVE");
}

/* ── SETTINGS ─────────────────────────────────────────────────────────────────── */
void xnet_ui_draw_settings(int cursor, const char* relay_ip, int relay_port,
                           uint32_t gate, uint32_t energy, int talking,
                           int debug_on, int mic_gain, uint32_t peak) {
    draw_header("SETTINGS");

    const char* labels[6] = { "RELAY IP", "RELAY PORT", "MIC SENSITIVITY",
                              "MIC GAIN", "DEBUG LOGGING", "BACK" };

    /* gate -> human label (low gate = picks up quiet speech = HIGH sensitivity) */
    const char* mic_mode =
        (gate == 0)      ? "OPEN MIC" :
        (gate <= 100000) ? "HIGH" :
        (gate <= 250000) ? "MEDIUM" :
        (gate <= 450000) ? "LOW" : "VERY LOW";

    char vals[6][48];
    snprintf(vals[0], sizeof(vals[0]), "%s", relay_ip);
    snprintf(vals[1], sizeof(vals[1]), "%d", relay_port);
    snprintf(vals[2], sizeof(vals[2]), "%s   < >", mic_mode);
    snprintf(vals[3], sizeof(vals[3]), "%d%%   < >", mic_gain);
    snprintf(vals[4], sizeof(vals[4]), "%s   < >", debug_on ? "ON" : "OFF");
    vals[5][0] = 0;   /* BACK */

    int y0 = 76, ih = 34, lx = 60, vx = 300;
    for (int i = 0; i < 6; i++) {
        int y   = y0 + i * ih;
        int sel = (i == cursor);
        if (sel) {
            fb_fill_rect(40, y - 6, SCREEN_W - 80, 26, COL_PANEL);
            fb_draw_rect_outline(40, y - 6, SCREEN_W - 80, 26, COL_ACCENT);
        }
        fb_draw_string(lx, y, labels[i], sel ? COL_ACCENT : COL_TEXT);
        if (vals[i][0]) {
            uint32_t vc = sel ? COL_ACCENT : COL_DIM;
            if (i == 4 && debug_on) vc = COL_ACCENT;
            fb_draw_string(vx, y, vals[i], vc);
        }
    }

    /* live mic meter — shown on the two mic rows so the level (and clipping)
       can be watched while tuning sensitivity or gain */
    if (cursor == 2 || cursor == 3) {
        const uint32_t METER_MAX = 1000000u;
        int bx = 60, bw = SCREEN_W - 120, by = y0 + 7 * ih + 16, bh = 20;
        int clipping = (peak >= 32000);   /* raw ADC near full-scale */

        if (cursor == 3)
            fb_draw_string(bx, by - 18,
                "Lower gain if CLIP shows while speaking normally", COL_DIM);
        else if (gate == 0)
            fb_draw_string(bx, by - 18, "OPEN MIC - always transmitting", COL_DIM);
        else
            fb_draw_string(bx, by - 18,
                "Speak: bar past the red line = transmitting", COL_DIM);

        fb_fill_rect(bx, by, bw, bh, COL_PANEL);
        fb_draw_rect_outline(bx, by, bw, bh, COL_DIM);
        uint32_t e  = (energy > METER_MAX) ? METER_MAX : energy;
        int      fw = (int)((e * (uint32_t)(bw - 2)) / METER_MAX);
        fb_fill_rect(bx + 1, by + 1, fw, bh - 2,
                     clipping ? COL_ERROR : (talking ? COL_ACCENT : COL_DIM));

        if (gate > 0) {
            uint32_t g   = (gate > METER_MAX) ? METER_MAX : gate;
            int      gxp = bx + 1 + (int)((g * (uint32_t)(bw - 2)) / METER_MAX);
            fb_fill_rect(gxp, by - 4, 2, bh + 8, COL_ERROR);
        }

        if (clipping)
            fb_draw_string(bx, by + bh + 6, "CLIP! mic too hot - lower gain", COL_ERROR);
        else
            fb_draw_string(bx, by + bh + 6, talking ? "TRANSMITTING" : "quiet",
                           talking ? COL_ACCENT : COL_DIM);
    } else if (cursor == 4) {
        int by = y0 + 7 * ih + 16;
        fb_draw_string(60, by,
            "ON writes a full per-frame trace to xnet.log (for bug reports).",
            COL_DIM);
        fb_draw_string(60, by + 14, "Leave OFF for normal use.", COL_DIM);
    }

    draw_footer("DPAD MOVE   < > ADJUST   A SELECT   B BACK");
}

/* ── ABOUT ────────────────────────────────────────────────────────────────────── */
typedef struct { const char* t; int h; } AboutLine;
void xnet_ui_draw_about(int* scroll) {
    static const AboutLine lines[] = {
        {"ABOUT XNET", 1},
        {"", 0},
        {"XNET is a zero-persistence, end-to-end encrypted", 0},
        {"communication system built for the original Xbox.", 0},
        {"", 0},
        {"Designed from the ground up for retail hardware, XNET", 0},
        {"enables up to four consoles anywhere in the world to", 0},
        {"communicate using modern encryption while preserving", 0},
        {"the spirit of the original Xbox era.", 0},
        {"", 0},
        {"FEATURES", 1},
        {"- Encrypted text chat", 0},
        {"- Real-time voice chat", 0},
        {"- Secure video chat", 0},
        {"- Encrypted file transfer", 0},
        {"- Zero-persistence architecture", 0},
        {"- Retail Xbox compatible", 0},
        {"- Xbox Communicator and Hawk headset support", 0},
        {"- PS2 EyeToy camera support", 0},
        {"", 0},
        {"PRIVACY BY DESIGN", 1},
        {"All messages, voice, video, and file transfers are", 0},
        {"encrypted end-to-end using AES-128. Session keys are", 0},
        {"derived locally from the room token and are never", 0},
        {"transmitted.", 0},
        {"", 0},
        {"The relay server cannot see your conversations. It", 0},
        {"only forwards encrypted packets and stores nothing.", 0},
        {"Sessions exist entirely in memory and disappear when", 0},
        {"the room ends.", 0},
        {"", 0},
        {"No accounts. No friend lists. No logs. No cloud", 0},
        {"storage.", 0},
        {"", 0},
        {"ARCHITECTURE", 1},
        {"- Client-side encryption", 0},
        {"- Zero-knowledge relay", 0},
        {"- Client-side audio mixing", 0},
        {"- Low-bandwidth operation", 0},
        {"- Built specifically for the 733 MHz Xbox hardware", 0},
        {"", 0},
        {"VERSION", 1},
        {"XNET v0.5.5 PUBLIC BETA", 0},
        {"Built with NXDK", 0},
        {"", 0},
        {"CREDITS", 1},
        {"Created by Tsardev", 0},
        {"Special thanks to the testers and preservation", 0},
        {"community keeping the original Xbox alive.", 0},
        {"", 0},
        {"\"Privacy by Design\"", 1},
    };
    int n = (int)(sizeof(lines) / sizeof(lines[0]));

    draw_header("ABOUT");

    int top = 44, lh = 11, bottom = SCREEN_H - 28;
    int visible   = (bottom - top) / lh;
    int maxscroll = (n > visible) ? (n - visible) : 0;
    if (*scroll > maxscroll) *scroll = maxscroll;
    if (*scroll < 0)         *scroll = 0;

    int y = top;
    for (int i = *scroll; i < n && y < bottom; i++) {
        fb_draw_string(24, y, lines[i].t, lines[i].h ? COL_ACCENT : COL_TEXT);
        y += lh;
    }

    if (maxscroll > 0) {
        char sh[16];
        snprintf(sh, sizeof(sh), "%d/%d", *scroll, maxscroll);
        fb_draw_string(SCREEN_W - 70, 36, sh, COL_DIM);
    }

    draw_footer("DPAD UP/DOWN SCROLL   B BACK");
}
