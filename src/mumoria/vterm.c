// Mumoria - a virtual terminal for each session. See vterm.h.

#include "vterm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__thread MuScreen *mu_vt_current = NULL;

static void out_append(MuScreen *s, const char *bytes, int len) {
    if (s->overflow) {
        return;
    }
    if (s->out_len + len > MU_OUT_LIMIT) {
        s->overflow = 1;
        return;
    }
    if (s->out_len + len > s->out_cap) {
        int cap = s->out_cap ? s->out_cap : 4096;
        while (cap < s->out_len + len) {
            cap *= 2;
        }
        s->out = realloc(s->out, (size_t) cap);
        s->out_cap = cap;
    }
    memcpy(s->out + s->out_len, bytes, (size_t) len);
    s->out_len += len;
}

static void wake(MuScreen *s) {
    if (s->wake_fd >= 0) {
        char b = 1;
        (void) !write(s->wake_fd, &b, 1);
    }
}

void mu_vt_init(MuScreen *s, int wake_fd) {
    memset(s, 0, sizeof(*s));
    pthread_mutex_init(&s->lock, NULL);
    memset(s->cell, ' ', sizeof(s->cell));
    memset(s->sent, ' ', sizeof(s->sent));
    memset(s->saved, ' ', sizeof(s->saved));
    s->full_redraw = 1;
    s->wake_fd = wake_fd;
}

void mu_vt_free(MuScreen *s) {
    free(s->out);
    s->out = NULL;
    pthread_mutex_destroy(&s->lock);
}

void mu_vt_set_attr(MuScreen *s, uint32_t attr) {
    s->cur_attr = attr;
}

void mu_vt_move(MuScreen *s, int y, int x) {
    if (y < 0) y = 0;
    if (y >= MU_GAME_ROWS) y = MU_GAME_ROWS - 1;
    if (x < 0) x = 0;
    if (x >= MU_COLS) x = MU_COLS - 1;
    s->cy = y;
    s->cx = x;
}

// The engine only ever writes Umoria's 24 rows; the chat pane is ours.
void mu_vt_addch(MuScreen *s, char ch) {
    unsigned char c = (unsigned char) ch;
    if (c < 32 || c > 126) {
        c = ' ';
    }
    pthread_mutex_lock(&s->lock);
    if (s->cy < MU_GAME_ROWS && s->cx < MU_COLS) {
        s->cell[s->cy][s->cx] = (char) c;
        s->attr[s->cy][s->cx] = s->cur_attr;
    }
    if (s->cx < MU_COLS - 1) {
        s->cx++;
    }
    pthread_mutex_unlock(&s->lock);
}

void mu_vt_addstr(MuScreen *s, const char *str) {
    for (; *str != '\0'; str++) {
        if (s->cx >= MU_COLS - 1 && *str != '\0') {
            // curses stops at the edge; Umoria truncates before this anyway
            mu_vt_addch(s, *str);
            break;
        }
        mu_vt_addch(s, *str);
    }
}

void mu_vt_clrtoeol(MuScreen *s) {
    pthread_mutex_lock(&s->lock);
    memset(&s->cell[s->cy][s->cx], ' ', (size_t) (MU_COLS - s->cx));
    memset(&s->attr[s->cy][s->cx], 0, sizeof(uint32_t) * (size_t) (MU_COLS - s->cx));
    pthread_mutex_unlock(&s->lock);
}

void mu_vt_clrtobot(MuScreen *s) {
    pthread_mutex_lock(&s->lock);
    memset(&s->cell[s->cy][s->cx], ' ', (size_t) (MU_COLS - s->cx));
    memset(&s->attr[s->cy][s->cx], 0, sizeof(uint32_t) * (size_t) (MU_COLS - s->cx));
    for (int y = s->cy + 1; y < MU_GAME_ROWS; y++) {
        memset(s->cell[y], ' ', MU_COLS);
        memset(s->attr[y], 0, sizeof(s->attr[y]));
    }
    pthread_mutex_unlock(&s->lock);
}

void mu_vt_clear(MuScreen *s) {
    pthread_mutex_lock(&s->lock);
    for (int y = 0; y < MU_GAME_ROWS; y++) {
        memset(s->cell[y], ' ', MU_COLS);
        memset(s->attr[y], 0, sizeof(s->attr[y]));
    }
    s->cy = s->cx = 0;
    pthread_mutex_unlock(&s->lock);
}

void mu_vt_save(MuScreen *s) {
    pthread_mutex_lock(&s->lock);
    memcpy(s->saved, s->cell, sizeof(s->saved));
    memcpy(s->saved_attr, s->attr, sizeof(s->saved_attr));
    pthread_mutex_unlock(&s->lock);
}

void mu_vt_restore(MuScreen *s) {
    pthread_mutex_lock(&s->lock);
    memcpy(s->cell, s->saved, sizeof(s->saved));
    memcpy(s->attr, s->saved_attr, sizeof(s->saved_attr));
    pthread_mutex_unlock(&s->lock);
}

void mu_vt_redraw(MuScreen *s) {
    pthread_mutex_lock(&s->lock);
    s->full_redraw = 1;
    pthread_mutex_unlock(&s->lock);
}

static void put_chat_rows(MuScreen *s) {
    for (int i = 0; i < MU_CHAT_ROWS; i++) {
        int y = MU_GAME_ROWS + i;
        memset(s->cell[y], ' ', MU_COLS);
        memset(s->attr[y], 0, sizeof(s->attr[y]));
        size_t n = strlen(s->chat[i]);
        memcpy(s->cell[y], s->chat[i], n > MU_COLS ? MU_COLS : n);
    }
    // a divider in the first chat row's place would cost a line; the
    // pane starts straight under Umoria's last row instead
    s->chat_dirty = 0;
}

static int same(MuScreen *s, int y, int x) {
    return s->cell[y][x] == s->sent[y][x] && s->attr[y][x] == s->sent_attr[y][x];
}

// SGR for a cell's look, sent only when it differs from the terminal's.
static void set_look(MuScreen *s, uint32_t attr) {
    if (attr == s->term_attr) {
        return;
    }
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "\033[0");
    if (attr & MU_ATTR_BOLD) {
        n += snprintf(buf + n, sizeof(buf) - (size_t) n, ";1");
    }
    if (attr & (1u << 25)) {
        n += snprintf(buf + n, sizeof(buf) - (size_t) n, ";38;5;%u", attr & 0xff);
    }
    if (attr & (1u << 26)) {
        n += snprintf(buf + n, sizeof(buf) - (size_t) n, ";48;5;%u", (attr >> 8) & 0xff);
    }
    n += snprintf(buf + n, sizeof(buf) - (size_t) n, "m");
    out_append(s, buf, n);
    s->term_attr = attr;
}

// Called with the lock held.
static void flush_locked(MuScreen *s) {
    char buf[64];

    if (s->chat_dirty) {
        put_chat_rows(s);
    }

    if (s->full_redraw) {
        out_append(s, "\033[0m\033[H\033[2J", 11);
        memset(s->sent, ' ', sizeof(s->sent));
        memset(s->sent_attr, 0, sizeof(s->sent_attr));
        s->term_attr = 0;
        s->full_redraw = 0;
    }

    for (int y = 0; y < MU_ROWS; y++) {
        int x = 0;
        while (x < MU_COLS) {
            if (same(s, y, x)) {
                x++;
                continue;
            }
            int start = x;
            // join runs separated by a few equal cells: one jump, not several
            int end = x;
            while (end < MU_COLS) {
                if (!same(s, y, end)) {
                    end++;
                    continue;
                }
                int gap = 0;
                while (end + gap < MU_COLS && gap < 4 && same(s, y, end + gap)) {
                    gap++;
                }
                if (end + gap >= MU_COLS || gap >= 4) {
                    break;
                }
                end += gap;
            }
            int n = snprintf(buf, sizeof(buf), "\033[%d;%dH", y + 1, start + 1);
            out_append(s, buf, n);
            for (int i = start; i < end; i++) {
                set_look(s, s->attr[y][i]);
                out_append(s, &s->cell[y][i], 1);
            }
            memcpy(&s->sent[y][start], &s->cell[y][start], (size_t) (end - start));
            memcpy(&s->sent_attr[y][start], &s->attr[y][start], sizeof(uint32_t) * (size_t) (end - start));
            x = end;
        }
    }

    set_look(s, 0);
    int n = snprintf(buf, sizeof(buf), "\033[%d;%dH", s->cy + 1, s->cx + 1);
    out_append(s, buf, n);
}

void mu_vt_flush(MuScreen *s) {
    pthread_mutex_lock(&s->lock);
    flush_locked(s);
    pthread_mutex_unlock(&s->lock);
    wake(s);
}

void mu_vt_bell(MuScreen *s) {
    pthread_mutex_lock(&s->lock);
    out_append(s, "\007", 1);
    pthread_mutex_unlock(&s->lock);
    wake(s);
}

void mu_vt_raw(MuScreen *s, const char *bytes, int len) {
    pthread_mutex_lock(&s->lock);
    out_append(s, bytes, len);
    pthread_mutex_unlock(&s->lock);
    wake(s);
}

void mu_vt_chat(MuScreen *s, const char *line) {
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < MU_CHAT_ROWS - 1; i++) {
        memcpy(s->chat[i], s->chat[i + 1], sizeof(s->chat[i]));
    }
    snprintf(s->chat[MU_CHAT_ROWS - 1], sizeof(s->chat[0]), "%s", line);
    for (char *p = s->chat[MU_CHAT_ROWS - 1]; *p != '\0'; p++) {
        if ((unsigned char) *p < 32 || (unsigned char) *p > 126) {
            *p = ' ';
        }
    }
    s->chat_dirty = 1;
    flush_locked(s);
    pthread_mutex_unlock(&s->lock);
    wake(s);
}

int mu_vt_take_output(MuScreen *s, char *buf, int max) {
    pthread_mutex_lock(&s->lock);
    int n = s->out_len < max ? s->out_len : max;
    if (n > 0) {
        memcpy(buf, s->out, (size_t) n);
        memmove(s->out, s->out + n, (size_t) (s->out_len - n));
        s->out_len -= n;
    }
    pthread_mutex_unlock(&s->lock);
    return n;
}
