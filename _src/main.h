/**
 * main.h
 * Shared types for XNET modules
 * Include this wherever XNetState or ChatMessage is needed.
 */

#ifndef XNET_MAIN_H
#define XNET_MAIN_H

#include <stdint.h>

#define MAX_SLOTS        4
#define TOKEN_LENGTH     8
#define MAX_MSG_LEN      128
#define CHAT_HISTORY_MAX 20

#include "xnet_replay.h"   /* xnet_replay_t — embedded in XNetState below */

typedef struct {
    char    text[MAX_MSG_LEN + 1];
    uint8_t slot;
    int     is_local;
} ChatMessage;

typedef enum {
    SCREEN_MAIN,
    SCREEN_TOKEN_DISPLAY,
    SCREEN_TOKEN_ENTRY,
    SCREEN_CHAT,
    SCREEN_ERROR,
    /* Secure Transfer */
    SCREEN_XFER_MENU,       /* SEND / RECEIVE chooser            */
    SCREEN_XFER_SEND_WAIT,  /* sender: token shown, await peer   */
    SCREEN_XFER_BROWSE,     /* sender: file manager              */
    SCREEN_XFER_CONFIRM,    /* sender: confirm picked file       */
    SCREEN_XFER_SENDING,    /* sender: progress bars             */
    SCREEN_XFER_RECV_WAIT,  /* receiver: "WAITING FOR FILES"     */
    SCREEN_XFER_RECEIVING,  /* receiver: progress bars           */
    SCREEN_XFER_RESULT,     /* either: SUCCESS / FAILED          */

    SCREEN_VIDEO_MENU,      /* START / JOIN chooser              */
    SCREEN_VIDEO,           /* live camera tile grid             */

    SCREEN_SETTINGS,        /* relay/mic/camera-test settings    */
    SCREEN_ABOUT,           /* about / credits                   */
} Screen;

/* Secure Transfer role */
typedef enum {
    XFER_NONE = 0,
    XFER_SEND,
    XFER_RECV,
} XferRole;

typedef struct {
    Screen       screen;
    int          sock;
    uint8_t      my_slot;
    char         token[TOKEN_LENGTH + 1];
    uint8_t      aes_key[16];
    int          peers_online[MAX_SLOTS];
    ChatMessage  history[CHAT_HISTORY_MAX];
    int          history_count;
    char         input_buf[MAX_MSG_LEN + 1];
    int          input_len;
    int          kbd_row;
    int          kbd_col;
    char         error_msg[128];
    uint8_t      xfer_role;   /* XferRole — routes token entry/join → transfer */
    uint8_t      video_mode;  /* 1 = this room is a Secure Video session        */
    uint8_t      video_cam_off; /* 1 = local camera muted (stop sending)        */
    xnet_replay_t replay;     /* anti-replay: counters, per-peer windows, session_id */
} XNetState;

/* Physical memory stats via the Xbox kernel (defined in main.c). */
void xnet_mem_query(unsigned long* free_kb, unsigned long* total_mb);

#endif /* XNET_MAIN_H */
