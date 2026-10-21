// Mumoria - a virtual terminal for each session.
//
// Umoria draws into a curses screen. On the server each session has one of
// these instead: the engine's curses calls (mu_curses.h) write into the
// session's grid, and mu_vt_flush() sends the client only the cells that
// changed since the last flush, as ANSI. Rows 0-23 are Umoria's own screen;
// rows 24 and up are the chat pane, written by any thread.

#pragma once

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MU_COLS 80
#define MU_GAME_ROWS 24
#define MU_CHAT_ROWS 6
#define MU_ROWS (MU_GAME_ROWS + MU_CHAT_ROWS)
#define MU_OUT_LIMIT (512 * 1024) // a client this far behind is dropped

typedef struct MuScreen {
    pthread_mutex_t lock;
    char cell[MU_ROWS][MU_COLS];
    char sent[MU_ROWS][MU_COLS];
    char saved[MU_GAME_ROWS][MU_COLS]; // terminalSaveScreen()
    int cy, cx;
    int full_redraw;

    char chat[MU_CHAT_ROWS][MU_COLS + 1];
    int chat_dirty;

    // output, drained by the session's I/O thread
    char *out;
    int out_len, out_cap;
    int overflow;
    int wake_fd; // written to wake the I/O thread
} MuScreen;

void mu_vt_init(MuScreen *s, int wake_fd);
void mu_vt_free(MuScreen *s);

// The screen the calling thread draws on (set by the session thread).
extern __thread MuScreen *mu_vt_current;

void mu_vt_move(MuScreen *s, int y, int x);
void mu_vt_addch(MuScreen *s, char ch);
void mu_vt_addstr(MuScreen *s, const char *str);
void mu_vt_clrtoeol(MuScreen *s);
void mu_vt_clrtobot(MuScreen *s);
void mu_vt_clear(MuScreen *s);
void mu_vt_save(MuScreen *s);
void mu_vt_restore(MuScreen *s);
void mu_vt_redraw(MuScreen *s); // ^R: send everything again
void mu_vt_flush(MuScreen *s);
void mu_vt_bell(MuScreen *s);
void mu_vt_raw(MuScreen *s, const char *bytes, int len); // telnet negotiation etc.

// Adds a line at the bottom of the chat pane, scrolling it. Any thread.
void mu_vt_chat(MuScreen *s, const char *line);

// Takes what is waiting to be sent; returns its length (0 if none).
int mu_vt_take_output(MuScreen *s, char *buf, int max);

#ifdef __cplusplus
}
#endif
