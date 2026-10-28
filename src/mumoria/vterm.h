// Mumoria - a virtual terminal for each session.
//
// Umoria draws into a curses screen. On the server each session has one of
// these instead: the engine's curses calls (mu_curses.h) write into the
// session's grid, and mu_vt_flush() sends the client only the cells that
// changed since the last flush, as ANSI. Rows 0-23, columns 0-79 are Umoria's
// own screen. The chat, written by any thread, is a boxed panel in columns
// 80-119 on a terminal at least 120 wide (its size comes from telnet NAWS),
// else the rows under Umoria's (24-29).

#pragma once

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MU_COLS 80       // Umoria's screen
#define MU_PANEL_COLS 40 // the chat panel beside it
#define MU_SCREEN_COLS (MU_COLS + MU_PANEL_COLS)
#define MU_GAME_ROWS 24
#define MU_CHAT_ROWS 6 // the chat pane under it, on a narrower terminal
#define MU_ROWS (MU_GAME_ROWS + MU_CHAT_ROWS)
#define MU_CHAT_KEEP 200 // lines of chat kept, wrapped again when the layout changes
#define MU_CHAT_LINE 256

// Where the chat goes (mumoria.conf chat_layout; a player's /panel).
enum { MU_LAYOUT_AUTO, MU_LAYOUT_SIDE, MU_LAYOUT_BOTTOM };
// The side panel's outline (chat_border; /panel border).
enum { MU_BORDER_ROUNDED, MU_BORDER_SINGLE, MU_BORDER_DOUBLE, MU_BORDER_HEAVY, MU_BORDER_ASCII, MU_BORDER_NONE, MU_BORDER_COUNT };
// The server's defaults (server_main.cpp, from mumoria.conf).
extern int mu_vt_default_layout;
extern int mu_vt_default_border;
extern int mu_vt_border_colour; // xterm 256; 0 for the terminal's own
int mu_vt_layout_by_name(const char *name); // -1 if none
int mu_vt_border_by_name(const char *name); // -1 if none
const char *mu_vt_layout_name(int layout);
const char *mu_vt_border_name(int border);
#define MU_OUT_LIMIT (512 * 1024) // a client this far behind is dropped

// A cell's look: an xterm 256-colour foreground and background, and bold.
// 0 is the terminal's own look.
#define MU_ATTR_BOLD (1u << 24)
#define MU_ATTR_FG(n) ((1u << 25) | (uint32_t) (n))
#define MU_ATTR_BG(n) ((1u << 26) | ((uint32_t) (n) << 8))

// One cell of an animation (fx.cpp): from t0 to t1 (mu_vt_now_ms), the
// level's square y, x shows ch in attr, over the map. ch MU_FX_TINT keeps the
// square's own character and only changes its look.
#define MU_FX_MAX 1024
#define MU_FX_TINT 1
typedef struct {
    int64_t t0, t1;
    int16_t y, x;
    char ch;
    uint32_t attr;
} MuFxCell;

// A sound for the client to play at t (fx.cpp queues them with the
// animations). It goes as telnet IAC SB MU_TELOPT_SOUND "name volume pitch"
// IAC SE, only to a client that answered the server's DO with WILL (net.c),
// so a plain telnet never sees it. Volume is 1-100; pitch is a percentage
// of the file's own (50-200, 100 as recorded), a creature's or a player's voice.
#define MU_TELOPT_SOUND 212
#define MU_SOUND_MAX 128
#define MU_SOUND_NAME 16
typedef struct {
    int64_t t;
    char name[MU_SOUND_NAME];
    int volume;
    int pitch;
} MuSound;

typedef struct MuScreen {
    pthread_mutex_t lock;
    char cell[MU_ROWS][MU_COLS];
    char sent[MU_ROWS][MU_SCREEN_COLS];
    char saved[MU_GAME_ROWS][MU_COLS]; // terminalSaveScreen()
    uint32_t attr[MU_ROWS][MU_COLS];
    uint32_t sent_attr[MU_ROWS][MU_SCREEN_COLS];
    uint32_t saved_attr[MU_GAME_ROWS][MU_COLS];
    uint32_t cur_attr;  // what mu_vt_addch writes with
    uint32_t term_attr; // what the client's terminal is set to
    int cy, cx;
    int full_redraw;

    char history[MU_CHAT_KEEP][MU_CHAT_LINE]; // the chat's last lines, as said
    int history_n;                            // lines ever said (history is a ring)
    int chat_dirty;
    int chat_scroll;                          // rows scrolled back from the newest (PgUp)
    char panel[MU_ROWS][MU_PANEL_COLS]; // the side panel; box pieces are MU_BOX_* codes
    uint32_t panel_attr[MU_ROWS][MU_PANEL_COLS];
    int term_cols, term_rows; // the client's terminal (telnet NAWS); 0 if not told
    int layout, border;       // this player's choice; -1 for the server's
    int side, rows_used;      // the layout drawn now: the side panel?, rows in use

    // Which cells show the map (drawn while map_drawing > 0): animations
    // are drawn only there, so never over a menu or a list.
    uint8_t mapcell[MU_GAME_ROWS][MU_COLS];
    uint8_t saved_mapcell[MU_GAME_ROWS][MU_COLS];
    int map_drawing;
    int map_set, map_top, map_bottom, map_left, map_right, map_row_prt, map_col_prt;
    MuFxCell fx[MU_FX_MAX];
    int fx_n;
    char ov_ch[MU_GAME_ROWS][MU_COLS]; // what is drawn over the map now; 0 = nothing
    uint32_t ov_attr[MU_GAME_ROWS][MU_COLS];
    int sound; // the client plays sounds (it said WILL MU_TELOPT_SOUND)
    MuSound sounds[MU_SOUND_MAX];
    int sound_n;

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
void mu_vt_set_attr(MuScreen *s, uint32_t attr);
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

// Adds a line at the bottom of the chat, scrolling it. Any thread.
void mu_vt_chat(MuScreen *s, const char *line);
// The player scrolls the chat (PgUp, PgDn, Home, End): older by a page,
// newer by a page, the oldest kept, the newest. Any thread.
enum { MU_CHAT_OLDER, MU_CHAT_NEWER, MU_CHAT_OLDEST, MU_CHAT_NEWEST };
void mu_vt_chat_scroll(MuScreen *s, int how);
// The client's terminal size (telnet NAWS): the layout may change.
void mu_vt_set_size(MuScreen *s, int cols, int rows);
// A player's choice: layout and border, -1 for the server's.
void mu_vt_set_panel(MuScreen *s, int layout, int border);
// Is the chat in the side panel now? Its text width (36 or 80).
int mu_vt_side(MuScreen *s);
int mu_vt_chat_width(MuScreen *s);

// Animations (fx.cpp makes them, the I/O thread plays them).
int64_t mu_vt_now_ms(void);
void mu_vt_map_drawing(MuScreen *s, int delta); // +1 before drawing map squares, -1 after
// The panel the map shows: a level square y, x is at screen row y - row_prt, column x - col_prt.
void mu_vt_map_panel(MuScreen *s, int top, int bottom, int left, int right, int row_prt, int col_prt);
void mu_vt_fx_add(MuScreen *s, const MuFxCell *cells, int n);
void mu_vt_fx_clear(MuScreen *s);
// A sound at t (mu_vt_now_ms); nothing unless the client plays sounds.
void mu_vt_sound(MuScreen *s, int64_t t, const char *name, int volume, int pitch);
// Draws the frame due now and sends the sounds due; returns the ms until the
// next change, -1 if none.
int mu_vt_fx_tick(MuScreen *s);

// Takes what is waiting to be sent; returns its length (0 if none).
int mu_vt_take_output(MuScreen *s, char *buf, int max);

#ifdef __cplusplus
}
#endif
