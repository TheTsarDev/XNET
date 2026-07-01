/**
 * XNET - Xbox Encrypted Chat
 * Dead Orbit Studios / Tsardev
 *
 * NXDK XBE client for XNET relay.
 * Provides encrypted text + voice chat for up to 4 original Xbox consoles.
 *
 * Build: NXDK (https://github.com/XboxDev/nxdk)
 * Target: Original Xbox (all revisions)
 */

#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

/* NXDK networking */
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <lwip/ip_addr.h>

/* stdlib */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* XNET modules */
#include "main.h"
#include "xnet_crypto.h"
#include "xnet_net.h"
#include "xnet_audio.h"
#include "xnet_ui.h"
#include "xnet_log.h"
#include "xnet_files.h"
#include "xnet_video.h"
#include "xnet_camera.h"

#include <nxdk/mount.h>

/* ── CONFIG ─────────────────────────────────────────────────────────────────── */
/* Built-in defaults — overridden at boot by D:\xnet.cfg if present.
 * Config format (plain text, FTP-editable, no rebuild needed):
 *     relay=192.168.1.50
 *     port=7777
 */
#define XNET_RELAY_HOST     "YOUR.RELAY.IP.HERE"
#define XNET_RELAY_PORT     7777
#define XNET_VERSION        "0.4.5.5"
/* MAX_SLOTS, TOKEN_LENGTH, MAX_MSG_LEN, CHAT_HISTORY_MAX defined in main.h */

static char g_relay_host[64] = XNET_RELAY_HOST;
static int  g_relay_port     = XNET_RELAY_PORT;

/* Load relay host/port from D:\xnet.cfg (D: = launch dir, automounted). */
static void load_config(void) {
    FILE* f = fopen("D:\\xnet.cfg", "r");
    if (!f) {
        xnet_logf("config: D:\\xnet.cfg not found, using built-in %s:%d",
                  g_relay_host, g_relay_port);
        return;
    }

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        /* strip CR/LF/whitespace at end */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                           line[len-1] == ' '  || line[len-1] == '\t')) {
            line[--len] = 0;
        }
        if (len == 0 || line[0] == '#') continue;

        if (strncmp(line, "relay=", 6) == 0 && line[6]) {
            strncpy(g_relay_host, line + 6, sizeof(g_relay_host) - 1);
            g_relay_host[sizeof(g_relay_host) - 1] = 0;
        } else if (strncmp(line, "port=", 5) == 0) {
            int p = atoi(line + 5);
            if (p > 0 && p < 65536) g_relay_port = p;
        } else if (strncmp(line, "mic_gate=", 9) == 0) {
            xnet_audio_set_gate((uint32_t)atoi(line + 9));
        } else if (strncmp(line, "mic_gain=", 9) == 0) {
            xnet_audio_set_gain(atoi(line + 9));
        } else if (strncmp(line, "debug_log=", 10) == 0) {
            xnet_log_set_verbose(atoi(line + 10));
        }
    }
    fclose(f);
    xnet_logf("config: loaded D:\\xnet.cfg -> relay=%s port=%d",
              g_relay_host, g_relay_port);
}

/* Rebuild the "RELAY: host:port" string shown on the main menu. */
static void refresh_relay_info(void) {
    char relay_info[96];
    snprintf(relay_info, sizeof(relay_info), "RELAY: %s:%d",
             g_relay_host, g_relay_port);
    xnet_ui_set_relay_info(relay_info);
}

/* Persist relay host/port and mic gate back to D:\xnet.cfg (written from the
   Settings screen). */
static void save_config(void) {
    FILE* f = fopen("D:\\xnet.cfg", "w");
    if (!f) {
        xnet_logf("config: save failed (could not open D:\\xnet.cfg)");
        return;
    }
    fprintf(f, "relay=%s\n", g_relay_host);
    fprintf(f, "port=%d\n",  g_relay_port);
    fprintf(f, "mic_gate=%lu\n", (unsigned long)xnet_audio_get_gate());
    fprintf(f, "mic_gain=%d\n", xnet_audio_get_gain());
    fprintf(f, "debug_log=%d\n", xnet_log_get_verbose());
    fclose(f);
    refresh_relay_info();   /* keep the menu's RELAY: line in sync */
    xnet_logf("config: saved relay=%s port=%d gate=%lu",
              g_relay_host, g_relay_port, (unsigned long)xnet_audio_get_gate());
}

/* What the on-screen keyboard is currently editing. */
enum { EDIT_TOKEN = 0, EDIT_RELAY_IP, EDIT_RELAY_PORT };
static int g_edit_target = EDIT_TOKEN;

/* ── PACKET TYPES ───────────────────────────────────────────────────────────── */
#define PKT_CREATE          0x01
#define PKT_TOKEN           0x02
#define PKT_JOIN            0x03
#define PKT_JOINED          0x04
#define PKT_PEER_JOINED     0x05
#define PKT_TEXT            0x06
#define PKT_TEXT_RELAY      0x07
#define PKT_VOICE           0x08
#define PKT_VOICE_RELAY     0x09
#define PKT_PING            0x0A
#define PKT_PONG            0x0B
#define PKT_PEER_LEFT       0x0C
#define PKT_END             0x0D
#define PKT_KICK            0x0E
#define PKT_ERROR           0x0F

/* ── SECURE TRANSFER PACKET TYPES ───────────────────────────────────────────────
 * Same blind-relay convention as TEXT/VOICE: client sends the base type, the
 * relay rebroadcasts it as the _RELAY variant to the other peer. */
#define PKT_FILE_META       0x10  /* sender→relay: encrypted {size,nchunks,name} */
#define PKT_FILE_META_RELAY 0x11
#define PKT_FILE_DATA       0x12  /* sender→relay: encrypted {u32 idx, bytes}    */
#define PKT_FILE_DATA_RELAY 0x13
#define PKT_FILE_PROG       0x14  /* receiver→relay: plain {u32 chunks_written}  */
#define PKT_FILE_PROG_RELAY 0x15
#define PKT_FILE_DONE       0x16  /* either→relay: plain {u8 ok, u8 errcode}     */
#define PKT_FILE_DONE_RELAY 0x17

/* ── SECURE VIDEO PACKET TYPES ──────────────────────────────────────────────────
 * One encrypted baseline-JPEG frame per packet, blind-relayed like VOICE. */
#define PKT_VIDEO           0x18  /* sender→relay: AES(JPEG frame) */
#define PKT_VIDEO_RELAY     0x19

/* plaintext chunk payload = 4096 data bytes; ciphertext = 16 IV + pad ≈ 4128 < cap */
#define FILE_CHUNK_DATA     4096
#define CHUNKS_PER_FRAME    6      /* chunks pushed per loop iteration while sending */
#define XFER_TIMEOUT_MS     20000  /* no peer activity during transfer = dead */

/* result error codes — surfaced to the user as "E0x" */
#define XERR_NONE     0
#define XERR_OPEN     1   /* sender: cannot open / read source file        */
#define XERR_CREATE   2   /* receiver: cannot create output file           */
#define XERR_WRITE    3   /* receiver: write failed (disk full?)           */
#define XERR_DECRYPT  4   /* crypt/unpad failure (wrong token / corruption)*/
#define XERR_SEQ      5   /* chunk arrived out of order                     */
#define XERR_NET      6   /* disconnect / timeout mid-transfer             */
#define XERR_SIZE     7   /* final byte count != advertised size           */
#define XERR_PROTO    8   /* malformed metadata / chunk                    */
#define XERR_ABORT    9   /* user aborted                                  */

/* types defined in main.h — included via xnet_ui.h → main.h */

static XNetState g_state;

/* ── SECURE TRANSFER STATE ──────────────────────────────────────────────────────
 * One transfer at a time. Scratch buffers are static (not stack) to keep loop
 * frames small on the Xbox. */
typedef struct {
    int           active;
    int           finished;       /* terminal status reached */
    int           success;
    int           err;
    FILE*         fp;
    char          basename[64];   /* file name only (no path)            */
    char          src_path[XFILES_PATH_MAX]; /* sender: full source path */
    char          out_path[XFILES_PATH_MAX]; /* receiver: dest path      */
    uint32_t      filesize;
    uint32_t      nchunks;
    uint32_t      sent_chunks;    /* sender: chunks pushed               */
    uint32_t      recv_chunks;    /* receiver: chunks written            */
    uint32_t      next_expected;  /* receiver: next chunk index expected */
    uint32_t      bytes_done;     /* receiver: bytes written             */
    uint32_t      peer_chunks;    /* sender: chunks receiver reported    */
    uint32_t      prog_every;     /* receiver: PROG cadence in chunks    */
    unsigned long last_activity_ms;
} FileXfer;

static FileXfer g_xfer;

/* chunk scratch: index(4) + data + PKCS7 slack / IV */
static uint8_t g_chunk_plain [4 + FILE_CHUNK_DATA + 32];
static uint8_t g_chunk_cipher[8 + 16 + 4 + FILE_CHUNK_DATA + 16 + 32];

/* ── SLOT COLORS (for chat attribution) ─────────────────────────────────────── */
/* RGBA colors per slot — slot 0 = green, 1 = cyan, 2 = yellow, 3 = magenta */
static const uint32_t SLOT_COLORS[MAX_SLOTS] = {
    0x00FF00FF,
    0x00FFFFFF,
    0xFFFF00FF,
    0xFF00FFFF,
};

/* ── ON-SCREEN KEYBOARD LAYOUT ───────────────────────────────────────────────── */
static const char* KBD_ROWS[] = {
    "ABCDEFGHIJ",
    "KLMNOPQRST",
    "UVWXYZ0123",
    "456789 .<-",   /* '<' = backspace, '-' = done/send */
};
#define KBD_ROW_COUNT 4
#define KBD_COL_COUNT 10

/* ── FORWARD DECLARATIONS ────────────────────────────────────────────────────── */
static void xnet_init(void);
static void xnet_loop(void);
static void handle_screen_main(void);
static void handle_screen_token_display(void);
static void handle_screen_token_entry(void);
static void handle_screen_chat(void);
static void do_create_room(void);
static void do_join_room(void);
static void send_text(const char* text);
static void recv_packets(void);
static void pump_voice(void);
static void append_chat(uint8_t slot, const char* text, int is_local);
static void derive_key_from_token(const char* token, uint8_t* key_out);

/* Query physical memory via the Xbox kernel. free_kb = free RAM in KB,
   total_mb = total physical RAM in MB. Pages are 4KB. */
void xnet_mem_query(unsigned long* free_kb, unsigned long* total_mb) {
    MM_STATISTICS s;
    s.Length = sizeof(s);
    MmQueryStatistics(&s);
    if (free_kb)  *free_kb  = (unsigned long)s.AvailablePages * 4;       /* pages * 4KB */
    if (total_mb) *total_mb = (unsigned long)(s.TotalPhysicalPages >> 8); /* pages / 256 = MB */
}

/* Secure Transfer */
static void handle_screen_xfer_menu(void);
static void handle_screen_xfer_send_wait(void);
static void handle_screen_xfer_browse(void);
static void handle_screen_xfer_confirm(void);
static void handle_screen_xfer_sending(void);
static void handle_screen_xfer_recv_wait(void);
static void handle_screen_xfer_receiving(void);
static void handle_screen_xfer_result(void);
static void do_xfer_send_create(void);
static void ensure_xnet_files_dir(void);
static void xfer_reset(void);
static int  xfer_peer_present(void);
static void xfer_begin_send(void);
static void xfer_send_batch(void);
static void xfer_send_prog(uint32_t chunks);
static void xfer_send_done(int ok, int code);
static void xfer_set_result(int ok, int code);
static void xfer_finish(int ok, int code);
static void sanitize_name(const char* in, char* out, int out_sz);
static int  pct_of(uint32_t num, uint32_t den);
static void put_u32(uint8_t* p, uint32_t v);
static uint32_t get_u32(const uint8_t* p);

/* Secure Video */
static void handle_screen_video_menu(void);
static void handle_screen_video(void);
static void handle_screen_settings(void);
static void handle_screen_about(void);
static void enter_video_session(void);

/* ═══════════════════════════════════════════════════════════════════════════════
 * ENTRY POINT
 * ═══════════════════════════════════════════════════════════════════════════════ */
void main(void) {
    /* logging first — only needs the kernel, survives any later hang */
    xnet_log_init();
    xnet_logf("main() entered");

    load_config();
    refresh_relay_info();

    BOOL vid_ok = XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    xnet_logf("XVideoSetMode 640x480x32 -> %s, FB=%p",
              vid_ok ? "OK" : "FAILED", (void*)XVideoGetFB());

    /* register XBLC driver BEFORE first flip: the first input poll inits
       SDL -> USB core -> device enumeration, and our driver must already
       be registered to win the probe for class 0x78 interfaces */
    xnet_logf("audio init (XBLC driver registration)...");
    int aud_ok = xnet_audio_init();
    xnet_logf("audio init done -> %d", aud_ok);

    /* register the camera driver into the now-initialised USB core, before
       SDL_Init triggers device enumeration so the probe can win the camera */
    xnet_camera_register();

    /* show something immediately — DHCP below can take several seconds */
    xnet_ui_draw_splash("STARTING NETWORK (DHCP)...");
    xnet_logf("splash drawn, presenting first frame...");
    xnet_ui_flip();
    xnet_logf("first frame presented");

    xnet_logf("nxNetInit (DHCP) starting...");
    int net_ok = xnet_net_init();   /* init lwIP / DHCP — blocks, non-fatal */
    xnet_logf("nxNetInit done -> %s", (net_ok == 0) ? "OK" : "FAILED");

    xnet_init();

    if (net_ok != 0) {
        /* DHCP didn't finish inside nxNetInit's window. The lease usually
         * lands a moment later, so a hard boot error is a false negative
         * (dismissing it and continuing is exactly what works). Go straight
         * to the menu; a real outage surfaces later as "CANNOT CONNECT". */
        xnet_logf("net: slow/failed at boot -> menu anyway (lease may complete)");
    }

    xnet_logf("entering main loop");
    xnet_loop();

    /* should never reach here */
    xnet_logf("main loop exited unexpectedly — rebooting");
    XReboot();
}

/* ── INIT ─────────────────────────────────────────────────────────────────────── */
static void xnet_init(void) {
    memset(&g_state, 0, sizeof(g_state));
    g_state.screen  = SCREEN_MAIN;
    g_state.sock    = -1;
    g_state.my_slot = 0xFF; /* unassigned */
}

/* ── MAIN LOOP ───────────────────────────────────────────────────────────────── */
static void xnet_loop(void) {
    while (1) {
        /* heartbeat: log free RAM + loop count. Cadence follows the debug
           toggle — every 5s when debug logging is on (so a freeze shows the
           RAM trend right up to the hang), every 60s otherwise (quiet for
           release, still enough to spot a trend). */
        {
            static unsigned long hb_ms = 0;
            static unsigned long loops = 0;
            loops++;
            unsigned long now = GetTickCount();
            unsigned long interval = xnet_log_get_verbose() ? 5000 : 60000;
            if (now - hb_ms >= interval) {
                hb_ms = now;
                unsigned long free_kb = 0, total_mb = 0;
                xnet_mem_query(&free_kb, &total_mb);
                xnet_logf("heartbeat: loops=%lu freeKB=%lu screen=%d",
                          loops, free_kb, g_state.screen);
            }
        }

        /* poll network if connected */
        if (g_state.sock >= 0) {
            recv_packets();
        }

        /* dispatch audio (open mic — always running in chat) */
        if (g_state.screen == SCREEN_CHAT && g_state.sock >= 0) {
            xnet_audio_tick(&g_state);
            if (g_state.screen == SCREEN_MAIN) {
                int hs = xnet_audio_headset_ok();
                xnet_ui_set_voice_status(hs ? "HEADSET: OK" : "HEADSET: --", hs);
            } else if (g_state.screen == SCREEN_CHAT) {
                if (!xnet_audio_headset_ok())
                    xnet_ui_set_voice_status("NO HEADSET", 0);
                else
                    xnet_ui_set_voice_status(xnet_audio_talking() ? "MIC*ON" : "MIC ON",
                                             xnet_audio_talking());
            }
        }

        /* Secure Video: push our camera frame + run voice alongside it */
        if (g_state.screen == SCREEN_VIDEO && g_state.sock >= 0) {
            if (!g_state.video_cam_off)
                xnet_video_capture_and_send(&g_state);
            xnet_audio_tick(&g_state);
        }

        /* dispatch screen */
        switch (g_state.screen) {
            case SCREEN_MAIN:          handle_screen_main();          break;
            case SCREEN_TOKEN_DISPLAY: handle_screen_token_display(); break;
            case SCREEN_TOKEN_ENTRY:   handle_screen_token_entry();   break;
            case SCREEN_CHAT:          handle_screen_chat();          break;
            case SCREEN_XFER_MENU:      handle_screen_xfer_menu();      break;
            case SCREEN_XFER_SEND_WAIT: handle_screen_xfer_send_wait(); break;
            case SCREEN_XFER_BROWSE:    handle_screen_xfer_browse();    break;
            case SCREEN_XFER_CONFIRM:   handle_screen_xfer_confirm();   break;
            case SCREEN_XFER_SENDING:   handle_screen_xfer_sending();   break;
            case SCREEN_XFER_RECV_WAIT: handle_screen_xfer_recv_wait(); break;
            case SCREEN_XFER_RECEIVING: handle_screen_xfer_receiving(); break;
            case SCREEN_XFER_RESULT:    handle_screen_xfer_result();    break;
            case SCREEN_VIDEO_MENU:     handle_screen_video_menu();     break;
            case SCREEN_VIDEO:          handle_screen_video();          break;
            case SCREEN_SETTINGS:       handle_screen_settings();       break;
            case SCREEN_ABOUT:          handle_screen_about();          break;
            case SCREEN_ERROR:
                xnet_ui_draw_error(g_state.error_msg);
                /* press B to go back to main */
                if (xnet_ui_button_pressed(BTN_B)) {
                    if (g_state.sock >= 0) {
                        closesocket(g_state.sock);
                        g_state.sock = -1;
                    }
                    xnet_init();
                }
                break;
        }

        /* second voice service this iteration — brackets the heavy video
           decode/draw above so the receive jitter buffer is fed promptly
           instead of in a burst on the next loop */
        pump_voice();

        xnet_ui_flip(); /* present frame */
    }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * SCREEN: MAIN MENU
 * ═══════════════════════════════════════════════════════════════════════════════ */
static void handle_screen_main(void) {
    static int cursor = 0;
    const char* items[] = { "CREATE ROOM", "JOIN ROOM", "VIDEO CHAT",
                            "FILE TRANSFER", "SETTINGS", "ABOUT", "QUIT" };
    const int   count   = 7;

    if (xnet_ui_dpad_up())   cursor = (cursor - 1 + count) % count;
    if (xnet_ui_dpad_down()) cursor = (cursor + 1) % count;

    if (xnet_ui_button_pressed(BTN_A)) {
        switch (cursor) {
            case 0:  /* CREATE ROOM */
                g_state.xfer_role = XFER_NONE;
                g_state.video_mode = 0;
                do_create_room();
                break;
            case 1:  /* JOIN ROOM */
                g_state.xfer_role = XFER_NONE;
                g_state.video_mode = 0;
                memset(g_state.token, 0, sizeof(g_state.token));
                g_state.kbd_row  = 0;
                g_state.kbd_col  = 0;
                g_state.input_len = 0;
                memset(g_state.input_buf, 0, sizeof(g_state.input_buf));
                g_edit_target = EDIT_TOKEN;
                g_state.screen = SCREEN_TOKEN_ENTRY;
                break;
            case 2:  /* VIDEO CHAT */
                g_state.xfer_role = XFER_NONE;
                g_state.video_mode = 0;
                g_state.video_cam_off = 0;
                g_state.screen = SCREEN_VIDEO_MENU;
                break;
            case 3:  /* FILE TRANSFER */
                ensure_xnet_files_dir();
                g_state.xfer_role = XFER_NONE;
                g_state.video_mode = 0;
                g_state.screen = SCREEN_XFER_MENU;
                break;
            case 4:  /* SETTINGS */
                g_state.screen = SCREEN_SETTINGS;
                break;
            case 5:  /* ABOUT */
                g_state.screen = SCREEN_ABOUT;
                break;
            case 6:  /* QUIT */
                xnet_camera_shutdown();   /* LED off before we leave to dash */
                XReboot();
                break;
        }
    }

    xnet_ui_draw_main_menu(items, count, cursor, XNET_VERSION);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * SCREEN: SETTINGS
 * ═══════════════════════════════════════════════════════════════════════════════ */
#define MIC_GATE_STEP  25000u
#define MIC_GATE_MAX   1000000u
#define MIC_GAIN_STEP  25
static void handle_screen_settings(void) {
    static int cursor = 0;
    const int  count  = 6;   /* IP, PORT, MIC SENS, MIC GAIN, DEBUG, BACK */

    if (xnet_ui_dpad_up())   cursor = (cursor - 1 + count) % count;
    if (xnet_ui_dpad_down()) cursor = (cursor + 1) % count;

    /* left/right tunes the mic threshold while that row is highlighted */
    if (cursor == 2) {
        uint32_t g = xnet_audio_get_gate();
        if (xnet_ui_dpad_left())  xnet_audio_set_gate(g >= MIC_GATE_STEP ? g - MIC_GATE_STEP : 0);
        if (xnet_ui_dpad_right()) xnet_audio_set_gate(g + MIC_GATE_STEP <= MIC_GATE_MAX ? g + MIC_GATE_STEP : MIC_GATE_MAX);
    }
    /* left/right tunes the mic input gain */
    if (cursor == 3) {
        int gn = xnet_audio_get_gain();
        if (xnet_ui_dpad_left())  xnet_audio_set_gain(gn - MIC_GAIN_STEP);
        if (xnet_ui_dpad_right()) xnet_audio_set_gain(gn + MIC_GAIN_STEP);
    }
    /* left/right toggles debug logging on its row */
    if (cursor == 4 && (xnet_ui_dpad_left() || xnet_ui_dpad_right())) {
        xnet_log_set_verbose(!xnet_log_get_verbose());
        save_config();
    }

    if (xnet_ui_button_pressed(BTN_A)) {
        switch (cursor) {
            case 0:  /* RELAY IP — edit on keyboard */
                g_edit_target  = EDIT_RELAY_IP;
                g_state.kbd_row = 0; g_state.kbd_col = 0;
                strncpy(g_state.input_buf, g_relay_host, sizeof(g_state.input_buf) - 1);
                g_state.input_buf[sizeof(g_state.input_buf) - 1] = 0;
                g_state.input_len = (int)strlen(g_state.input_buf);
                g_state.screen = SCREEN_TOKEN_ENTRY;
                break;
            case 1:  /* RELAY PORT — edit on keyboard */
                g_edit_target  = EDIT_RELAY_PORT;
                g_state.kbd_row = 0; g_state.kbd_col = 0;
                snprintf(g_state.input_buf, sizeof(g_state.input_buf), "%d", g_relay_port);
                g_state.input_len = (int)strlen(g_state.input_buf);
                g_state.screen = SCREEN_TOKEN_ENTRY;
                break;
            case 2:  /* MIC SENSITIVITY — A saves */
            case 3:  /* MIC GAIN — A saves */
                save_config();
                break;
            case 4:  /* DEBUG LOGGING — A toggles */
                xnet_log_set_verbose(!xnet_log_get_verbose());
                save_config();
                break;
            case 5:  /* BACK */
                save_config();
                g_state.screen = SCREEN_MAIN;
                break;
        }
    }

    if (xnet_ui_button_pressed(BTN_B)) {
        save_config();
        g_state.screen = SCREEN_MAIN;
        return;
    }

    xnet_audio_monitor();   /* update live mic level for the meter */
    xnet_ui_draw_settings(cursor, g_relay_host, g_relay_port,
                          xnet_audio_get_gate(), xnet_audio_last_energy(),
                          xnet_audio_talking(), xnet_log_get_verbose(),
                          xnet_audio_get_gain(), xnet_audio_last_peak());
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * SCREEN: ABOUT
 * ═══════════════════════════════════════════════════════════════════════════════ */
static void handle_screen_about(void) {
    static int scroll = 0;
    if (xnet_ui_dpad_up())   scroll = (scroll > 0) ? scroll - 1 : 0;
    if (xnet_ui_dpad_down()) scroll = scroll + 1;   /* clamped inside draw */

    if (xnet_ui_button_pressed(BTN_B)) {
        scroll = 0;
        g_state.screen = SCREEN_MAIN;
        return;
    }

    xnet_ui_draw_about(&scroll);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * SCREEN: TOKEN DISPLAY (host waiting for peer)
 * ═══════════════════════════════════════════════════════════════════════════════ */
static void handle_screen_token_display(void) {
    /* B = cancel, go back to main */
    if (xnet_ui_button_pressed(BTN_B)) {
        if (g_state.sock >= 0) {
            xnet_net_send_pkt(g_state.sock, PKT_END, 0, NULL, 0);
            closesocket(g_state.sock);
            g_state.sock = -1;
        }
        xnet_init();
        return;
    }

    /* peer count check happens in recv_packets → PEER_JOINED → transitions to SCREEN_CHAT */
    xnet_ui_draw_token_display(g_state.token, g_state.peers_online, MAX_SLOTS);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * SCREEN: TOKEN ENTRY (joiner types token)
 * ═══════════════════════════════════════════════════════════════════════════════ */
static void handle_screen_token_entry(void) {
    int max_len = (g_edit_target == EDIT_RELAY_IP)   ? (int)sizeof(g_relay_host) - 1 :
                  (g_edit_target == EDIT_RELAY_PORT) ? 5 : TOKEN_LENGTH;

    /* d-pad navigate keyboard */
    if (xnet_ui_dpad_up())    g_state.kbd_row = (g_state.kbd_row - 1 + KBD_ROW_COUNT) % KBD_ROW_COUNT;
    if (xnet_ui_dpad_down())  g_state.kbd_row = (g_state.kbd_row + 1) % KBD_ROW_COUNT;
    if (xnet_ui_dpad_left())  g_state.kbd_col = (g_state.kbd_col - 1 + KBD_COL_COUNT) % KBD_COL_COUNT;
    if (xnet_ui_dpad_right()) g_state.kbd_col = (g_state.kbd_col + 1) % KBD_COL_COUNT;

    if (xnet_ui_button_pressed(BTN_A)) {
        char ch = KBD_ROWS[g_state.kbd_row][g_state.kbd_col];
        if (ch == '<') {
            if (g_state.input_len > 0) {
                g_state.input_len--;
                g_state.input_buf[g_state.input_len] = 0;
            }
        } else if (ch == '-') {
            /* submit, action depends on what we're editing */
            if (g_edit_target == EDIT_TOKEN) {
                if (g_state.input_len == TOKEN_LENGTH) {
                    strncpy(g_state.token, g_state.input_buf, TOKEN_LENGTH);
                    g_state.token[TOKEN_LENGTH] = 0;
                    do_join_room();
                }
            } else if (g_edit_target == EDIT_RELAY_IP) {
                if (g_state.input_len > 0) {
                    strncpy(g_relay_host, g_state.input_buf, sizeof(g_relay_host) - 1);
                    g_relay_host[sizeof(g_relay_host) - 1] = 0;
                    save_config();
                }
                g_state.screen = SCREEN_SETTINGS;
                return;
            } else { /* EDIT_RELAY_PORT */
                if (g_state.input_len > 0) {
                    int p = atoi(g_state.input_buf);
                    if (p > 0 && p < 65536) { g_relay_port = p; save_config(); }
                }
                g_state.screen = SCREEN_SETTINGS;
                return;
            }
        } else if (ch == ' ') {
            /* space — no-op for any of these fields */
        } else if (g_state.input_len < max_len) {
            g_state.input_buf[g_state.input_len++] = ch;
            g_state.input_buf[g_state.input_len]   = 0;
        }
    }

    /* X = quick backspace */
    if (xnet_ui_button_pressed(BTN_X)) {
        if (g_state.input_len > 0) {
            g_state.input_len--;
            g_state.input_buf[g_state.input_len] = 0;
        }
    }

    /* B = back */
    if (xnet_ui_button_pressed(BTN_B)) {
        if (g_edit_target == EDIT_TOKEN) {
            xnet_init();
        } else {
            g_state.screen = SCREEN_SETTINGS;   /* cancel edit, no save */
        }
        return;
    }

    const char* title  = (g_edit_target == EDIT_TOKEN) ? "JOIN ROOM" : "SETTINGS";
    const char* prompt = (g_edit_target == EDIT_RELAY_IP)   ? "ENTER RELAY IP" :
                         (g_edit_target == EDIT_RELAY_PORT) ? "ENTER RELAY PORT" :
                                                              "ENTER ROOM TOKEN";
    xnet_ui_draw_keyboard(KBD_ROWS, KBD_ROW_COUNT, KBD_COL_COUNT,
                          g_state.kbd_row, g_state.kbd_col,
                          g_state.input_buf, title, prompt);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * SCREEN: CHAT ROOM
 * ═══════════════════════════════════════════════════════════════════════════════ */
static void handle_screen_chat(void) {
    /* d-pad navigate keyboard for text input */
    if (xnet_ui_dpad_up())    g_state.kbd_row = (g_state.kbd_row - 1 + KBD_ROW_COUNT) % KBD_ROW_COUNT;
    if (xnet_ui_dpad_down())  g_state.kbd_row = (g_state.kbd_row + 1) % KBD_ROW_COUNT;
    if (xnet_ui_dpad_left())  g_state.kbd_col = (g_state.kbd_col - 1 + KBD_COL_COUNT) % KBD_COL_COUNT;
    if (xnet_ui_dpad_right()) g_state.kbd_col = (g_state.kbd_col + 1) % KBD_COL_COUNT;

    if (xnet_ui_button_pressed(BTN_A)) {
        char ch = KBD_ROWS[g_state.kbd_row][g_state.kbd_col];
        if (ch == '<') {
            if (g_state.input_len > 0) {
                g_state.input_len--;
                g_state.input_buf[g_state.input_len] = 0;
            }
        } else if (ch == '-') {
            /* send message */
            if (g_state.input_len > 0) {
                send_text(g_state.input_buf);
                append_chat(g_state.my_slot, g_state.input_buf, 1);
                memset(g_state.input_buf, 0, sizeof(g_state.input_buf));
                g_state.input_len = 0;
            }
        } else if (g_state.input_len < MAX_MSG_LEN) {
            g_state.input_buf[g_state.input_len++] = (ch == ' ') ? ' ' : ch;
            g_state.input_buf[g_state.input_len]   = 0;
        }
    }

    /* X = quick backspace (the on-screen '<' key still works too) */
    if (xnet_ui_button_pressed(BTN_X)) {
        if (g_state.input_len > 0) {
            g_state.input_len--;
            g_state.input_buf[g_state.input_len] = 0;
        }
    }

    /* Start = leave room */
    if (xnet_ui_button_pressed(BTN_START)) {
        if (g_state.my_slot == 0) {
            xnet_net_send_pkt(g_state.sock, PKT_END, g_state.my_slot, NULL, 0);
        }
        closesocket(g_state.sock);
        g_state.sock = -1;
        xnet_init();
        return;
    }

    xnet_ui_draw_chat(
        g_state.history,
        g_state.history_count,
        g_state.input_buf,
        g_state.peers_online,
        g_state.my_slot,
        SLOT_COLORS,
        KBD_ROWS,
        KBD_ROW_COUNT,
        KBD_COL_COUNT,
        g_state.kbd_row,
        g_state.kbd_col
    );
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * SECURE TRANSFER
 * ═══════════════════════════════════════════════════════════════════════════════ */

/* ── byte / math helpers ─────────────────────────────────────────────────────── */
static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static uint32_t get_u32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | ((uint32_t)p[3]);
}

static int pct_of(uint32_t num, uint32_t den) {
    if (den == 0)   return 100;
    if (num >= den) return 100;
    return (int)((num * 100UL) / den);
}

/* Keep only the basename, drop FATX-illegal characters, cap at 42 chars. */
static void sanitize_name(const char* in, char* out, int out_sz) {
    const char* base = in;
    for (const char* p = in; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;

    int cap = out_sz - 1;
    if (cap > 42) cap = 42;   /* FATX filename limit */
    int o = 0;
    for (const char* p = base; *p && o < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>'  || c == '|' || c == '\\'|| c == '/' || c == ':')
            out[o++] = '_';
        else
            out[o++] = (char)c;
    }
    out[o] = 0;
    if (o == 0) {
        strncpy(out, "received.bin", out_sz - 1);
        out[out_sz - 1] = 0;
    }
}

/* ── lifecycle helpers ───────────────────────────────────────────────────────── */
static void ensure_xnet_files_dir(void) {
    /* E: is normally mounted at boot; mount defensively just in case. */
    if (!nxIsDriveMounted('E'))
        nxMountDrive('E', "\\Device\\Harddisk0\\Partition1");
    CreateDirectoryA("E:\\XNET FILES", NULL); /* harmless if it already exists */
}

static void xfer_reset(void) {
    if (g_xfer.fp) { fclose(g_xfer.fp); g_xfer.fp = NULL; }
    memset(&g_xfer, 0, sizeof(g_xfer));
}

static int xfer_peer_present(void) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (i == g_state.my_slot) continue;
        if (g_state.peers_online[i]) return 1;
    }
    return 0;
}

/* Local teardown + result screen. Does NOT notify the peer. */
static void xfer_set_result(int ok, int code) {
    if (g_xfer.fp) { fclose(g_xfer.fp); g_xfer.fp = NULL; }
    g_xfer.active   = 0;
    g_xfer.finished = 1;
    g_xfer.success  = ok ? 1 : 0;
    g_xfer.err      = ok ? XERR_NONE : code;
    g_state.screen  = SCREEN_XFER_RESULT;
}

static void xfer_send_done(int ok, int code) {
    uint8_t p[2];
    p[0] = (uint8_t)(ok ? 1 : 0);
    p[1] = (uint8_t)code;
    if (g_state.sock >= 0)
        xnet_net_send_pkt(g_state.sock, PKT_FILE_DONE, g_state.my_slot, p, 2);
}

/* Decide the outcome locally AND tell the peer, then show the result. */
static void xfer_finish(int ok, int code) {
    xfer_send_done(ok, code);
    xfer_set_result(ok, code);
}

static void xfer_send_prog(uint32_t chunks) {
    uint8_t p[4];
    put_u32(p, chunks);
    if (g_state.sock >= 0)
        xnet_net_send_pkt(g_state.sock, PKT_FILE_PROG, g_state.my_slot, p, 4);
}

/* ── sender: open relay room (mirrors do_create_room, different screen) ───────── */
static void do_xfer_send_create(void) {
    g_state.sock = xnet_net_connect(g_relay_host, g_relay_port);
    if (g_state.sock < 0) {
        strncpy(g_state.error_msg, "CANNOT CONNECT TO RELAY", sizeof(g_state.error_msg));
        g_state.screen = SCREEN_ERROR;
        return;
    }
    g_state.xfer_role = XFER_SEND;
    xfer_reset();
    xnet_net_send_pkt(g_state.sock, PKT_CREATE, 0, NULL, 0);
    g_state.screen = SCREEN_XFER_SEND_WAIT;
    /* token + key arrive via recv_packets → PKT_TOKEN (screen unchanged) */
}

/* ── sender: build + send META, arm the push loop ─────────────────────────────── */
static void xfer_begin_send(void) {
    g_xfer.fp = fopen(g_xfer.src_path, "rb");
    if (!g_xfer.fp) { xfer_set_result(0, XERR_OPEN); return; }

    fseek(g_xfer.fp, 0, SEEK_END);
    long sz = ftell(g_xfer.fp);
    fseek(g_xfer.fp, 0, SEEK_SET);
    if (sz < 0) { fclose(g_xfer.fp); g_xfer.fp = NULL; xfer_set_result(0, XERR_OPEN); return; }

    g_xfer.filesize    = (uint32_t)sz;
    g_xfer.nchunks     = (uint32_t)((sz + FILE_CHUNK_DATA - 1) / FILE_CHUNK_DATA);
    if (g_xfer.nchunks == 0) g_xfer.nchunks = 1; /* 0-byte file → one empty chunk */
    g_xfer.sent_chunks = 0;
    g_xfer.peer_chunks = 0;
    g_xfer.finished    = 0;

    /* META plaintext: [u32 size][u32 nchunks][u16 namelen][name] */
    uint8_t meta[4 + 4 + 2 + XFILES_NAME_MAX];
    int nl = (int)strlen(g_xfer.basename);
    if (nl > XFILES_NAME_MAX - 1) nl = XFILES_NAME_MAX - 1;
    put_u32(meta,     g_xfer.filesize);
    put_u32(meta + 4, g_xfer.nchunks);
    meta[8] = (uint8_t)((nl >> 8) & 0xFF);
    meta[9] = (uint8_t)(nl & 0xFF);
    memcpy(meta + 10, g_xfer.basename, nl);
    int mplain = 10 + nl;

    uint8_t mcipher[8 + 16 + 4 + 4 + 2 + XFILES_NAME_MAX + 16 + 32];
    int clen = xnet_replay_seal(&g_state.replay, g_state.aes_key,
                                XNET_STREAM_FILE, g_state.my_slot,
                                meta, mplain, mcipher, sizeof(mcipher), 1);
    if (clen < 0) { fclose(g_xfer.fp); g_xfer.fp = NULL; xfer_set_result(0, XERR_DECRYPT); return; }
    if (xnet_net_send_pkt(g_state.sock, PKT_FILE_META, g_state.my_slot,
                          mcipher, (uint16_t)clen) != 0) {
        fclose(g_xfer.fp); g_xfer.fp = NULL; xfer_set_result(0, XERR_NET); return;
    }

    g_xfer.active           = 1;
    g_xfer.last_activity_ms = GetTickCount();
    g_state.screen          = SCREEN_XFER_SENDING;
    xnet_logf("xfer: META sent name='%s' size=%u nchunks=%u",
              g_xfer.basename, g_xfer.filesize, g_xfer.nchunks);
}

/* ── sender: push up to CHUNKS_PER_FRAME chunks this frame ─────────────────────── */
static void xfer_send_batch(void) {
    int sent_this_frame = 0;
    while (g_xfer.sent_chunks < g_xfer.nchunks && sent_this_frame < CHUNKS_PER_FRAME) {
        size_t rd = fread(g_chunk_plain + 4, 1, FILE_CHUNK_DATA, g_xfer.fp);
        if (rd == 0 && g_xfer.nchunks > 1) {
            /* short read before all chunks accounted for = read error */
            xfer_finish(0, XERR_OPEN);
            return;
        }
        put_u32(g_chunk_plain, g_xfer.sent_chunks);

        int clen = xnet_replay_seal(&g_state.replay, g_state.aes_key,
                       XNET_STREAM_FILE, g_state.my_slot,
                       g_chunk_plain, (int)(4 + rd),
                       g_chunk_cipher, sizeof(g_chunk_cipher), 1);
        if (clen < 0) { xfer_finish(0, XERR_DECRYPT); return; }

        if (xnet_net_send_pkt(g_state.sock, PKT_FILE_DATA, g_state.my_slot,
                              g_chunk_cipher, (uint16_t)clen) != 0) {
            xnet_logf("xfer: send_pkt failed at chunk %u -> E%d",
                      g_xfer.sent_chunks, XERR_NET);
            xfer_finish(0, XERR_NET);
            return;
        }
        g_xfer.sent_chunks++;
        sent_this_frame++;
        g_xfer.last_activity_ms = GetTickCount();
    }
}

/* ═══ SCREEN: SEND / RECEIVE chooser ═══ */
static void handle_screen_xfer_menu(void) {
    static int cursor = 0;
    if (xnet_ui_dpad_up())   cursor = (cursor - 1 + 2) % 2;
    if (xnet_ui_dpad_down()) cursor = (cursor + 1) % 2;

    if (xnet_ui_button_pressed(BTN_A)) {
        if (cursor == 0) {
            do_xfer_send_create();
        } else {
            g_state.xfer_role = XFER_RECV;
            memset(g_state.token, 0, sizeof(g_state.token));
            g_state.kbd_row = 0;
            g_state.kbd_col = 0;
            g_state.input_len = 0;
            memset(g_state.input_buf, 0, sizeof(g_state.input_buf));
            g_edit_target = EDIT_TOKEN;
            g_state.screen = SCREEN_TOKEN_ENTRY;
        }
        return;
    }
    if (xnet_ui_button_pressed(BTN_B)) {
        g_state.xfer_role = XFER_NONE;
        g_state.screen = SCREEN_MAIN;
        return;
    }
    xnet_ui_draw_xfer_menu(cursor);
}

/* ═══ SCREEN: sender token shown, waiting for receiver ═══ */
static void handle_screen_xfer_send_wait(void) {
    if (xnet_ui_button_pressed(BTN_B)) {
        if (g_state.sock >= 0) {
            xnet_net_send_pkt(g_state.sock, PKT_END, g_state.my_slot, NULL, 0);
            closesocket(g_state.sock);
            g_state.sock = -1;
        }
        xfer_reset();
        xnet_init();
        return;
    }

    int peer = xfer_peer_present();
    if (peer && xnet_ui_rtrigger_pressed()) {
        xnet_files_open_root();
        g_state.screen = SCREEN_XFER_BROWSE;
        return;
    }
    xnet_ui_draw_xfer_send_wait(g_state.token, peer);
}

/* ═══ SCREEN: sender file browser ═══ */
static void handle_screen_xfer_browse(void) {
    if (xnet_ui_dpad_up())   xnet_files_move(-1);
    if (xnet_ui_dpad_down()) xnet_files_move(1);

    /* A or Right Trigger: enter a directory, or pick a file */
    if (xnet_ui_button_pressed(BTN_A) || xnet_ui_rtrigger_pressed()) {
        if (xnet_files_cursor_is_file()) {
            if (xnet_files_selected_path(g_xfer.src_path, sizeof(g_xfer.src_path))) {
                const XFileList* fl = xnet_files_get();
                const XFileEntry* e = &fl->entries[fl->cursor];
                sanitize_name(e->name, g_xfer.basename, sizeof(g_xfer.basename));
                g_xfer.filesize = (uint32_t)e->size;
                g_state.screen = SCREEN_XFER_CONFIRM;
            }
        } else {
            xnet_files_enter();
        }
        return;
    }

    if (xnet_ui_button_pressed(BTN_B)) {
        const XFileList* fl = xnet_files_get();
        if (fl->path[0] == 0)
            g_state.screen = SCREEN_XFER_SEND_WAIT;  /* at drive root → back out */
        else
            xnet_files_up();
        return;
    }

    xnet_ui_draw_browser(xnet_files_get());
}

/* ═══ SCREEN: confirm picked file ═══ */
static void handle_screen_xfer_confirm(void) {
    if (xnet_ui_button_pressed(BTN_A)) {
        xfer_begin_send();
        return;
    }
    if (xnet_ui_button_pressed(BTN_B)) {
        g_state.screen = SCREEN_XFER_BROWSE;
        return;
    }
    xnet_ui_draw_xfer_confirm(g_xfer.basename, g_xfer.filesize);
}

/* ═══ SCREEN: sender progress ═══ */
static void handle_screen_xfer_sending(void) {
    if (xnet_ui_button_pressed(BTN_START)) {
        xnet_logf("xfer: send aborted by user at %u/%u",
                  g_xfer.sent_chunks, g_xfer.nchunks);
        xfer_finish(0, XERR_ABORT);
        return;
    }

    if (g_xfer.sent_chunks < g_xfer.nchunks) {
        xfer_send_batch();
        if (g_xfer.active && g_xfer.sent_chunks >= g_xfer.nchunks)
            xnet_logf("xfer: all %u chunks pushed, awaiting receiver", g_xfer.nchunks);
    } else {
        /* All chunks pushed. The receiver's DONE (in recv_packets) normally
         * ends it. If that DONE races/drops but the receiver already acked
         * every chunk via PROG, count it a success instead of timing out. */
        if (g_xfer.peer_chunks >= g_xfer.nchunks) {
            xnet_logf("xfer: receiver acked all chunks (no DONE) -> SUCCESS");
            xfer_set_result(1, XERR_NONE);
            return;
        }
        if ((GetTickCount() - g_xfer.last_activity_ms) > XFER_TIMEOUT_MS) {
            xnet_logf("xfer: timeout waiting on receiver (acked %u/%u) -> E%d",
                      g_xfer.peer_chunks, g_xfer.nchunks, XERR_NET);
            xfer_finish(0, XERR_NET);
            return;
        }
    }

    int up   = pct_of(g_xfer.sent_chunks, g_xfer.nchunks);
    int down = pct_of(g_xfer.peer_chunks, g_xfer.nchunks);
    xnet_ui_draw_xfer_progress("SENDING", g_xfer.basename, up, down);
}

/* ═══ SCREEN: receiver idle ("WAITING FOR FILES") ═══ */
static void handle_screen_xfer_recv_wait(void) {
    if (xnet_ui_button_pressed(BTN_B)) {
        if (g_state.sock >= 0) {
            xnet_net_send_pkt(g_state.sock, PKT_END, g_state.my_slot, NULL, 0);
            closesocket(g_state.sock);
            g_state.sock = -1;
        }
        xfer_reset();
        xnet_init();
        return;
    }
    xnet_ui_draw_xfer_wait_files();
}

/* ═══ SCREEN: receiver progress ═══ */
static void handle_screen_xfer_receiving(void) {
    if (xnet_ui_button_pressed(BTN_START)) {
        xfer_finish(0, XERR_ABORT);
        return;
    }
    if ((GetTickCount() - g_xfer.last_activity_ms) > XFER_TIMEOUT_MS) {
        xfer_finish(0, XERR_NET);
        return;
    }
    int up   = pct_of(g_xfer.next_expected, g_xfer.nchunks);
    int down = pct_of(g_xfer.recv_chunks,   g_xfer.nchunks);
    xnet_ui_draw_xfer_progress("RECEIVING", g_xfer.basename, up, down);
}

/* ═══ SCREEN: SUCCESS / FAILED ═══ */
static void handle_screen_xfer_result(void) {
    if (xnet_ui_button_pressed(BTN_A) || xnet_ui_button_pressed(BTN_B)) {
        if (g_state.sock >= 0) {
            xnet_net_send_pkt(g_state.sock, PKT_END, g_state.my_slot, NULL, 0);
            closesocket(g_state.sock);
            g_state.sock = -1;
        }
        xfer_reset();
        xnet_init();
        return;
    }
    xnet_ui_draw_xfer_result(g_xfer.success, g_xfer.err, g_xfer.basename);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * SECURE VIDEO
 * ═══════════════════════════════════════════════════════════════════════════════ */

/* Bring up the camera + clear the grid, then show the live session. Called from
   recv_packets when the room is fully joined (host on peer-join, joiner on
   PKT_JOINED). */
static void enter_video_session(void) {
    xnet_video_reset();
    g_state.video_cam_off = 0;
    /* Always init — even when already streaming. init() sets g_want_frames
       (which gates frame assembly), captures the session baseline, and
       re-kicks the sensor. Skipping it when streaming was the bug that left
       Secure Video stuck on "connecting" while DEBUG CAMERA worked. */
    int rc = xnet_camera_init();
    xnet_logf("video: camera init -> %d (streaming=%d)",
              rc, xnet_camera_streaming());
    g_state.screen = SCREEN_VIDEO;
}

/* START / JOIN chooser. */
static void handle_screen_video_menu(void) {
    static int cursor = 0;
    if (xnet_ui_dpad_up())   cursor = (cursor - 1 + 2) % 2;
    if (xnet_ui_dpad_down()) cursor = (cursor + 1) % 2;

    if (xnet_ui_button_pressed(BTN_A)) {
        g_state.video_mode = 1;
        g_state.xfer_role  = XFER_NONE;
        if (cursor == 0) {
            /* START = create a room, show token, wait for peers (like CREATE) */
            do_create_room();
        } else {
            /* JOIN = enter a token (reuses the keyboard) */
            memset(g_state.token, 0, sizeof(g_state.token));
            g_state.kbd_row = 0;
            g_state.kbd_col = 0;
            g_state.input_len = 0;
            memset(g_state.input_buf, 0, sizeof(g_state.input_buf));
            g_edit_target = EDIT_TOKEN;
            g_state.screen = SCREEN_TOKEN_ENTRY;
        }
        return;
    }
    if (xnet_ui_button_pressed(BTN_B)) {
        g_state.video_mode = 0;
        g_state.screen = SCREEN_MAIN;
        return;
    }
    xnet_ui_draw_video_menu(cursor);
}

/* Live tiled session. Capture+send and voice are pumped from the main loop. */
static void handle_screen_video(void) {
    /* Y toggles the local camera (stops sending; shows CAMERA OFF on our tile) */
    if (xnet_ui_button_pressed(BTN_Y)) {
        g_state.video_cam_off = !g_state.video_cam_off;
        if (g_state.video_cam_off)
            xnet_video_clear_slot(g_state.my_slot);
    }

    /* Start = leave the session */
    if (xnet_ui_button_pressed(BTN_START)) {
        if (g_state.sock >= 0) {
            xnet_net_send_pkt(g_state.sock, PKT_END, g_state.my_slot, NULL, 0);
            closesocket(g_state.sock);
            g_state.sock = -1;
        }
        xnet_camera_shutdown();
        xnet_video_reset();
        xnet_init();
        return;
    }

    xnet_video_decode_pending(pump_voice);   /* decode peer frames; service voice between slots */
    int cam_ok = xnet_camera_streaming() && !g_state.video_cam_off;
    xnet_ui_draw_video_grid(g_state.my_slot, g_state.peers_online, cam_ok,
                            xnet_audio_headset_ok(), xnet_audio_talking());
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * NETWORK ACTIONS
 * ═══════════════════════════════════════════════════════════════════════════════ */
static void do_create_room(void) {
    g_state.sock = xnet_net_connect(g_relay_host, g_relay_port);
    if (g_state.sock < 0) {
        strncpy(g_state.error_msg, "CANNOT CONNECT TO RELAY", sizeof(g_state.error_msg));
        g_state.screen = SCREEN_ERROR;
        return;
    }

    xnet_net_send_pkt(g_state.sock, PKT_CREATE, 0, NULL, 0);
    g_state.screen = SCREEN_TOKEN_DISPLAY;
    /* token arrives via recv_packets → PKT_TOKEN handler */
}

static void do_join_room(void) {
    g_state.sock = xnet_net_connect(g_relay_host, g_relay_port);
    if (g_state.sock < 0) {
        strncpy(g_state.error_msg, "CANNOT CONNECT TO RELAY", sizeof(g_state.error_msg));
        g_state.screen = SCREEN_ERROR;
        return;
    }

    /* send JOIN with token as payload */
    xnet_net_send_pkt(g_state.sock, PKT_JOIN, 0,
                      (uint8_t*)g_state.token, TOKEN_LENGTH);
    /* slot assignment arrives via recv_packets → PKT_JOINED handler */
}

static void send_text(const char* text) {
    uint8_t  plaintext[MAX_MSG_LEN];
    uint8_t  ciphertext[8 + MAX_MSG_LEN + 16 + 16 + 32]; /* seq + msg + IV + pad + MAC */
    int      plain_len, cipher_len;
    (void)plaintext;

    plain_len  = (int)strlen(text);
    cipher_len = xnet_replay_seal(&g_state.replay, g_state.aes_key,
                                  XNET_STREAM_TEXT, g_state.my_slot,
                                  (uint8_t*)text, plain_len,
                                  ciphertext, sizeof(ciphertext), 0);
    if (cipher_len > 0) {
        xnet_net_send_pkt(g_state.sock, PKT_TEXT, g_state.my_slot,
                          ciphertext, cipher_len);
    }
}

/* Service the time-critical voice path: drain the network (feeds the speaker
   ring and stashes video) and run the mic encoder/sender. Cheap and idempotent
   when there's nothing pending. Called multiple times per loop iteration —
   especially around the long video decode — so voice never waits behind a
   ~12ms+ JPEG decode and the receive jitter buffer doesn't overflow in bursts. */
static void pump_voice(void) {
    if (g_state.sock < 0) return;
    if (g_state.screen != SCREEN_VIDEO && g_state.screen != SCREEN_CHAT) return;
    recv_packets();
    xnet_audio_tick(&g_state);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * PACKET RECEIVER / HANDLER
 * ═══════════════════════════════════════════════════════════════════════════════ */
static void recv_packets(void) {
    uint8_t  header[4];
    uint8_t  payload[8192];
    int      type, slot, payload_len;

    /* non-blocking peek — drain all available packets this frame */
    while (xnet_net_recv_pkt(g_state.sock, &type, &slot, payload, sizeof(payload), &payload_len) > 0) {
        switch (type) {

            case PKT_TOKEN: {
                /* host receives room token (+ 16-byte session nonce from relay) */
                if (payload_len >= TOKEN_LENGTH) {
                    memcpy(g_state.token, payload, TOKEN_LENGTH);
                    g_state.token[TOKEN_LENGTH] = 0;
                    g_state.my_slot = 0;
                    g_state.peers_online[0] = 1;
                    derive_key_from_token(g_state.token, g_state.aes_key);
                    /* fresh room -> fresh counters/windows */
                    xnet_replay_init(&g_state.replay);
                    if (payload_len >= TOKEN_LENGTH + XNET_SESSION_ID_LEN)
                        xnet_replay_set_session(&g_state.replay,
                                                payload + TOKEN_LENGTH);
                }
                break;
            }

            case PKT_JOINED: {
                /* joiner receives slot assignment (+ 16-byte session nonce) */
                if (payload_len >= 1) {
                    g_state.my_slot = payload[0];
                    g_state.peers_online[g_state.my_slot] = 1;
                    derive_key_from_token(g_state.token, g_state.aes_key);
                    xnet_replay_init(&g_state.replay);
                    if (payload_len >= 1 + XNET_SESSION_ID_LEN)
                        xnet_replay_set_session(&g_state.replay, payload + 1);
                    if (g_state.xfer_role == XFER_RECV) {
                        /* Secure Transfer receiver: idle until a file arrives */
                        xfer_reset();   /* clears g_xfer; xfer_role is in g_state */
                        g_state.screen = SCREEN_XFER_RECV_WAIT;
                    } else if (g_state.video_mode) {
                        /* Secure Video joiner: go live */
                        enter_video_session();
                    } else {
                        g_state.screen = SCREEN_CHAT;
                        g_state.kbd_row  = 0;
                        g_state.kbd_col  = 0;
                        /* clear the token the joiner just typed so it doesn't
                         * linger in the chat input line */
                        g_state.input_len    = 0;
                        g_state.input_buf[0] = 0;
                    }
                }
                break;
            }

            case PKT_PEER_JOINED: {
                if (payload_len >= 1) {
                    uint8_t peer_slot = payload[0];
                    if (peer_slot < MAX_SLOTS) {
                        g_state.peers_online[peer_slot] = 1;
                    }
                    /* host transitions to chat (or video) on first peer join */
                    if (g_state.screen == SCREEN_TOKEN_DISPLAY) {
                        if (g_state.video_mode) {
                            enter_video_session();
                        } else {
                            g_state.screen = SCREEN_CHAT;
                            g_state.kbd_row = 0;
                            g_state.kbd_col = 0;
                            g_state.input_len    = 0;
                            g_state.input_buf[0] = 0;
                        }
                    }
                }
                break;
            }

            case PKT_TEXT_RELAY: {
                /* decrypt and display incoming message */
                uint8_t plaintext[MAX_MSG_LEN + 1];
                int plain_len = xnet_replay_open(&g_state.replay, g_state.aes_key,
                                                 XNET_STREAM_TEXT, (uint8_t)slot,
                                                 payload, payload_len,
                                                 plaintext, sizeof(plaintext), 0);
                if (plain_len > 0) {
                    plaintext[plain_len] = 0;
                    append_chat((uint8_t)slot, (char*)plaintext, 0);
                }
                break;
            }

            case PKT_VOICE_RELAY: {
                /* decrypt and queue for audio playback */
                uint8_t pcm_buf[1024];
                int pcm_len = xnet_replay_open(&g_state.replay, g_state.aes_key,
                                               XNET_STREAM_VOICE, (uint8_t)slot,
                                               payload, payload_len,
                                               pcm_buf, sizeof(pcm_buf), 0);
                if (pcm_len > 0) {
                    xnet_audio_queue_playback((uint8_t)slot, pcm_buf, pcm_len);
                }
                break;
            }

            case PKT_VIDEO_RELAY: {
                /* a peer's camera frame: decrypt JPEG, hand to the decoder.
                   Static scratch keeps this large buffer off the stack. */
                static uint8_t s_vid_jpeg[8192];
                static int s_rx = 0;
                if (g_state.video_mode && slot != g_state.my_slot) {
                    int jlen = xnet_replay_open(&g_state.replay, g_state.aes_key,
                                                XNET_STREAM_VIDEO, (uint8_t)slot,
                                                payload, payload_len,
                                                s_vid_jpeg, sizeof(s_vid_jpeg), 1);
                    if ((s_rx++ % 30) == 0)
                        xnet_vlogf("video: rx slot=%d frames=%d paylen=%d jlen=%d",
                                  slot, s_rx, payload_len, jlen);
                    if (jlen > 0)
                        xnet_video_stash_frame(slot, s_vid_jpeg, jlen); /* decode deferred to draw loop */
                }
                break;
            }

            case PKT_FILE_META_RELAY: {
                /* receiver: incoming transfer header */
                if (g_state.xfer_role != XFER_RECV) break;

                uint8_t meta[16 + XFILES_NAME_MAX];
                int mlen = xnet_replay_open(&g_state.replay, g_state.aes_key,
                                            XNET_STREAM_FILE, (uint8_t)slot,
                                            payload, payload_len,
                                            meta, sizeof(meta), 1);
                if (mlen < 10) {
                    xnet_logf("xfer: META decrypt failed (mlen=%d) -> E%d", mlen, XERR_DECRYPT);
                    xfer_finish(0, XERR_DECRYPT);
                    break;
                }

                uint32_t size    = get_u32(meta);
                uint32_t nchunks = get_u32(meta + 4);
                int      namelen = (meta[8] << 8) | meta[9];
                if (namelen < 0) namelen = 0;
                if (namelen > XFILES_NAME_MAX - 1) namelen = XFILES_NAME_MAX - 1;
                if (10 + namelen > mlen) { xfer_finish(0, XERR_PROTO); break; }

                char raw[XFILES_NAME_MAX];
                memcpy(raw, meta + 10, namelen);
                raw[namelen] = 0;

                xfer_reset();
                sanitize_name(raw, g_xfer.basename, sizeof(g_xfer.basename));

                ensure_xnet_files_dir();
                snprintf(g_xfer.out_path, sizeof(g_xfer.out_path),
                         "E:\\XNET FILES\\%s", g_xfer.basename);
                g_xfer.fp = fopen(g_xfer.out_path, "wb");
                if (!g_xfer.fp) {
                    xnet_logf("xfer: cannot create '%s' -> E%d", g_xfer.out_path, XERR_CREATE);
                    xfer_set_result(0, XERR_CREATE);
                    break;
                }

                g_xfer.active        = 1;
                g_xfer.finished      = 0;
                g_xfer.filesize      = size;
                g_xfer.nchunks       = nchunks ? nchunks : 1;
                g_xfer.recv_chunks   = 0;
                g_xfer.next_expected = 0;
                g_xfer.bytes_done    = 0;
                g_xfer.prog_every    = g_xfer.nchunks / 20;
                if (g_xfer.prog_every == 0) g_xfer.prog_every = 1;
                g_xfer.last_activity_ms = GetTickCount();
                g_state.screen = SCREEN_XFER_RECEIVING;
                xnet_logf("xfer: META rx name='%s' size=%u nchunks=%u -> receiving",
                          g_xfer.basename, size, nchunks);
                break;
            }

            case PKT_FILE_DATA_RELAY: {
                /* receiver: one file chunk */
                if (g_state.xfer_role != XFER_RECV || !g_xfer.active) break;

                uint8_t buf[4 + FILE_CHUNK_DATA + 32];
                int dlen = xnet_replay_open(&g_state.replay, g_state.aes_key,
                                            XNET_STREAM_FILE, (uint8_t)slot,
                                            payload, payload_len,
                                            buf, sizeof(buf), 1);
                if (dlen < 4) { xfer_finish(0, XERR_DECRYPT); break; }

                uint32_t idx = get_u32(buf);
                if (idx != g_xfer.next_expected) {
                    xnet_logf("xfer: SEQ want %u got %u -> E%d",
                              g_xfer.next_expected, idx, XERR_SEQ);
                    xfer_finish(0, XERR_SEQ);
                    break;
                }
                if (g_xfer.recv_chunks == 0)
                    xnet_logf("xfer: first chunk rx (%d data bytes)", dlen - 4);

                int datalen = dlen - 4;
                if (datalen > 0) {
                    if (fwrite(buf + 4, 1, (size_t)datalen, g_xfer.fp) != (size_t)datalen) {
                        xnet_logf("xfer: write failed at chunk %u -> E%d",
                                  g_xfer.recv_chunks, XERR_WRITE);
                        xfer_finish(0, XERR_WRITE);
                        break;
                    }
                }
                g_xfer.bytes_done += (uint32_t)datalen;
                g_xfer.recv_chunks++;
                g_xfer.next_expected++;
                g_xfer.last_activity_ms = GetTickCount();

                int last = (g_xfer.recv_chunks >= g_xfer.nchunks);
                if (last || (g_xfer.recv_chunks % g_xfer.prog_every) == 0)
                    xfer_send_prog(g_xfer.recv_chunks);

                if (last) {
                    fflush(g_xfer.fp);
                    if (g_xfer.bytes_done != g_xfer.filesize) {
                        xnet_logf("xfer: size mismatch got=%u want=%u -> E%d",
                                  g_xfer.bytes_done, g_xfer.filesize, XERR_SIZE);
                        xfer_finish(0, XERR_SIZE);
                    } else {
                        xnet_logf("xfer: complete '%s' %u bytes -> SUCCESS",
                                  g_xfer.basename, g_xfer.bytes_done);
                        xfer_finish(1, XERR_NONE);
                    }
                }
                break;
            }

            case PKT_FILE_PROG_RELAY: {
                /* sender: receiver reported how many chunks it has written */
                if (g_state.xfer_role != XFER_SEND) break;
                if (payload_len >= 4) {
                    g_xfer.peer_chunks      = get_u32(payload);
                    g_xfer.last_activity_ms = GetTickCount();
                }
                break;
            }

            case PKT_FILE_DONE_RELAY: {
                /* either side: peer signalled terminal status */
                if (g_state.xfer_role == XFER_NONE) break;
                if (!g_xfer.finished) {
                    int ok   = (payload_len >= 1) ? (payload[0] != 0) : 0;
                    int code = (payload_len >= 2) ? payload[1] : XERR_NET;
                    xnet_logf("xfer: DONE rx ok=%d code=%d", ok, code);
                    xfer_set_result(ok, ok ? XERR_NONE : code);
                }
                break;
            }

            case PKT_PEER_LEFT: {
                if (payload_len >= 1) {
                    uint8_t peer_slot = payload[0];
                    if (peer_slot < MAX_SLOTS) {
                        g_state.peers_online[peer_slot] = 0;
                        xnet_video_clear_slot(peer_slot); /* drop their tile */
                    }
                }
                /* peer vanished mid-transfer → fail it */
                if (g_xfer.active &&
                    (g_state.screen == SCREEN_XFER_SENDING ||
                     g_state.screen == SCREEN_XFER_RECEIVING)) {
                    xfer_set_result(0, XERR_NET);
                }
                break;
            }

            case PKT_KICK: {
                /* room ended or kicked — return to main */
                closesocket(g_state.sock);
                g_state.sock = -1;
                strncpy(g_state.error_msg, "SESSION ENDED", sizeof(g_state.error_msg));
                g_state.screen = SCREEN_ERROR;
                break;
            }

            case PKT_PING: {
                xnet_net_send_pkt(g_state.sock, PKT_PONG, g_state.my_slot, NULL, 0);
                break;
            }

            case PKT_ERROR: {
                payload[payload_len < 127 ? payload_len : 127] = 0;
                strncpy(g_state.error_msg, (char*)payload, sizeof(g_state.error_msg));
                g_state.screen = SCREEN_ERROR;
                break;
            }

            default:
                break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * HELPERS
 * ═══════════════════════════════════════════════════════════════════════════════ */
static void append_chat(uint8_t slot, const char* text, int is_local) {
    if (g_state.history_count < CHAT_HISTORY_MAX) {
        ChatMessage* m = &g_state.history[g_state.history_count++];
        m->slot     = slot;
        m->is_local = is_local;
        strncpy(m->text, text, MAX_MSG_LEN);
        m->text[MAX_MSG_LEN] = 0;
    } else {
        /* scroll — drop oldest */
        memmove(&g_state.history[0], &g_state.history[1],
                sizeof(ChatMessage) * (CHAT_HISTORY_MAX - 1));
        ChatMessage* m = &g_state.history[CHAT_HISTORY_MAX - 1];
        m->slot     = slot;
        m->is_local = is_local;
        strncpy(m->text, text, MAX_MSG_LEN);
        m->text[MAX_MSG_LEN] = 0;
    }
}

/**
 * Derive AES-128 key from token string using SHA-256, take first 16 bytes.
 * Both consoles run this independently — key never transmitted.
 */
static void derive_key_from_token(const char* token, uint8_t* key_out) {
    xnet_crypto_sha256_16(token, (int)strlen(token), key_out);
}
