// Mumoria - the curses calls Umoria's ui_io.cpp makes, on the session's
// virtual terminal (vterm.h) instead of a real one. Only ui_io.cpp includes
// this, in place of curses.h, when MUMORIA is defined.

#pragma once

#include "vterm.h"

#ifndef ERR
#define ERR (-1)
#endif
#ifndef OK
#define OK 0
#endif

#define LINES MU_GAME_ROWS
#define COLS MU_COLS

static inline int mu_c_move(int y, int x) {
    if (y < 0 || y >= MU_GAME_ROWS || x < 0 || x >= MU_COLS) {
        return ERR;
    }
    mu_vt_move(mu_vt_current, y, x);
    return OK;
}

static inline int mu_c_mvaddch(int y, int x, char ch) {
    if (mu_c_move(y, x) == ERR) {
        return ERR;
    }
    mu_vt_addch(mu_vt_current, ch);
    return OK;
}

static inline int mu_c_mvaddstr(int y, int x, const char *s) {
    if (mu_c_move(y, x) == ERR) {
        return ERR;
    }
    mu_vt_addstr(mu_vt_current, s);
    return OK;
}

#define move(y, x) mu_c_move((y), (x))
#define mvaddch(y, x, c) mu_c_mvaddch((y), (x), (char) (c))
#define mvaddstr(y, x, s) mu_c_mvaddstr((y), (x), (s))
#define addch(c) (mu_vt_addch(mu_vt_current, (char) (c)), OK)
#define addstr(s) (mu_vt_addstr(mu_vt_current, (s)), OK)
#define clrtoeol() mu_vt_clrtoeol(mu_vt_current)
#define clrtobot() mu_vt_clrtobot(mu_vt_current)
#define clear() (mu_vt_clear(mu_vt_current), OK)
#define refresh() (mu_vt_flush(mu_vt_current), OK)
#define getyx(w, y, x) ((y) = mu_vt_current->cy, (x) = mu_vt_current->cx)
