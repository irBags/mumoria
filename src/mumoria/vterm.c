// Mumoria - a virtual terminal for each session. See vterm.h.

#include "vterm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

__thread MuScreen *mu_vt_current = NULL;

int mu_vt_default_layout = MU_LAYOUT_AUTO;
int mu_vt_default_border = MU_BORDER_ROUNDED;
int mu_vt_border_colour = 245; // grey

// The side panel's box pieces, as cell codes, and how each style draws them.
enum { MU_BOX_H = 0x10, MU_BOX_V, MU_BOX_TL, MU_BOX_TR, MU_BOX_BL, MU_BOX_BR };
static const char *BOX[MU_BORDER_COUNT][6] = {
    {"\xe2\x94\x80", "\xe2\x94\x82", "\xe2\x95\xad", "\xe2\x95\xae", "\xe2\x95\xb0", "\xe2\x95\xaf"}, // rounded
    {"\xe2\x94\x80", "\xe2\x94\x82", "\xe2\x94\x8c", "\xe2\x94\x90", "\xe2\x94\x94", "\xe2\x94\x98"}, // single
    {"\xe2\x95\x90", "\xe2\x95\x91", "\xe2\x95\x94", "\xe2\x95\x97", "\xe2\x95\x9a", "\xe2\x95\x9d"}, // double
    {"\xe2\x94\x81", "\xe2\x94\x83", "\xe2\x94\x8f", "\xe2\x94\x93", "\xe2\x94\x97", "\xe2\x94\x9b"}, // heavy
    {"-", "|", "+", "+", "+", "+"},                                                                   // ascii
    {" ", " ", " ", " ", " ", " "},                                                                   // none
};
static const char *BORDER_NAMES[MU_BORDER_COUNT] = {"rounded", "single", "double", "heavy", "ascii", "none"};
static const char *LAYOUT_NAMES[3] = {"auto", "side", "bottom"};

int mu_vt_layout_by_name(const char *name) {
    for (int i = 0; i < 3; i++) {
        if (strcmp(name, LAYOUT_NAMES[i]) == 0) {
            return i;
        }
    }
    return -1;
}

int mu_vt_border_by_name(const char *name) {
    for (int i = 0; i < MU_BORDER_COUNT; i++) {
        if (strcmp(name, BORDER_NAMES[i]) == 0) {
            return i;
        }
    }
    return -1;
}

const char *mu_vt_layout_name(int layout) {
    return layout >= 0 && layout < 3 ? LAYOUT_NAMES[layout] : "auto";
}

const char *mu_vt_border_name(int border) {
    return border >= 0 && border < MU_BORDER_COUNT ? BORDER_NAMES[border] : "rounded";
}

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
    s->layout = s->border = -1;
    s->chat_dirty = 1;
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
        s->mapcell[s->cy][s->cx] = s->map_drawing > 0;
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
    memset(&s->mapcell[s->cy][s->cx], s->map_drawing > 0, (size_t) (MU_COLS - s->cx));
    pthread_mutex_unlock(&s->lock);
}

void mu_vt_clrtobot(MuScreen *s) {
    pthread_mutex_lock(&s->lock);
    memset(&s->cell[s->cy][s->cx], ' ', (size_t) (MU_COLS - s->cx));
    memset(&s->attr[s->cy][s->cx], 0, sizeof(uint32_t) * (size_t) (MU_COLS - s->cx));
    memset(&s->mapcell[s->cy][s->cx], 0, (size_t) (MU_COLS - s->cx));
    for (int y = s->cy + 1; y < MU_GAME_ROWS; y++) {
        memset(s->cell[y], ' ', MU_COLS);
        memset(s->attr[y], 0, sizeof(s->attr[y]));
        memset(s->mapcell[y], 0, MU_COLS);
    }
    pthread_mutex_unlock(&s->lock);
}

void mu_vt_clear(MuScreen *s) {
    pthread_mutex_lock(&s->lock);
    for (int y = 0; y < MU_GAME_ROWS; y++) {
        memset(s->cell[y], ' ', MU_COLS);
        memset(s->attr[y], 0, sizeof(s->attr[y]));
    }
    memset(s->mapcell, 0, sizeof(s->mapcell));
    s->cy = s->cx = 0;
    pthread_mutex_unlock(&s->lock);
}

void mu_vt_save(MuScreen *s) {
    pthread_mutex_lock(&s->lock);
    memcpy(s->saved, s->cell, sizeof(s->saved));
    memcpy(s->saved_attr, s->attr, sizeof(s->saved_attr));
    memcpy(s->saved_mapcell, s->mapcell, sizeof(s->saved_mapcell));
    pthread_mutex_unlock(&s->lock);
}

void mu_vt_restore(MuScreen *s) {
    pthread_mutex_lock(&s->lock);
    memcpy(s->cell, s->saved, sizeof(s->saved));
    memcpy(s->attr, s->saved_attr, sizeof(s->saved_attr));
    memcpy(s->mapcell, s->saved_mapcell, sizeof(s->mapcell));
    pthread_mutex_unlock(&s->lock);
}

void mu_vt_redraw(MuScreen *s) {
    pthread_mutex_lock(&s->lock);
    s->full_redraw = 1;
    pthread_mutex_unlock(&s->lock);
}

static int want_side(MuScreen *s) {
    int layout = s->layout >= 0 ? s->layout : mu_vt_default_layout;
    if (layout == MU_LAYOUT_SIDE) {
        return 1;
    }
    return layout == MU_LAYOUT_AUTO && s->term_cols >= MU_SCREEN_COLS;
}

static int border_of(MuScreen *s) {
    int b = s->border >= 0 ? s->border : mu_vt_default_border;
    return b >= 0 && b < MU_BORDER_COUNT ? b : MU_BORDER_ROUNDED;
}

// The side panel's text width: inside the box, a space either side.
#define SIDE_TEXT (MU_PANEL_COLS - 4)

// The last `want` rows of the chat, wrapped at width (going on rows indented
// by two), oldest first. Returns how many.
static int chat_rows(MuScreen *s, int width, int want, char rows[MU_ROWS][MU_COLS + 1]) {
    int count = 0, first = s->history_n > MU_CHAT_KEEP ? s->history_n - MU_CHAT_KEEP : 0;
    for (int n = first; n < s->history_n; n++) {
        const char *p = s->history[n % MU_CHAT_KEEP];
        int indent = 0;
        do {
            int room = width - indent;
            int len = (int) strlen(p);
            if (len > room) {
                len = room;
                while (len > room / 2 && p[len] != ' ') {
                    len--; // break at the last space, unless it is far back
                }
                if (p[len] != ' ') {
                    len = room;
                }
            }
            if (count == want) {
                memmove(rows[0], rows[1], sizeof(rows[0]) * (size_t) (want - 1));
                count--;
            }
            memset(rows[count], ' ', (size_t) indent);
            memcpy(rows[count] + indent, p, (size_t) len);
            rows[count][indent + len] = '\0';
            count++;
            p += len;
            while (*p == ' ') {
                p++;
            }
            indent = 2;
        } while (*p != '\0');
    }
    return count;
}

static void panel_put(MuScreen *s, int y, int x, char ch, uint32_t attr) {
    s->panel[y][x] = ch;
    s->panel_attr[y][x] = attr;
}

// The chat, where the layout puts it.
static void put_chat_rows(MuScreen *s) {
    int side = want_side(s);
    int rows = MU_ROWS;
    if (side) {
        rows = s->term_rows > 0 ? s->term_rows : MU_GAME_ROWS;
        rows = rows < MU_GAME_ROWS ? MU_GAME_ROWS : rows > MU_ROWS ? MU_ROWS : rows;
    }
    if (side != s->side || rows != s->rows_used) {
        s->side = side;
        s->rows_used = rows;
        s->full_redraw = 1;
    }
    char text[MU_ROWS][MU_COLS + 1];
    int want = side ? rows - 2 : MU_CHAT_ROWS;
    int have = chat_rows(s, side ? SIDE_TEXT : MU_COLS, want, text);

    for (int y = MU_GAME_ROWS; y < MU_ROWS; y++) {
        memset(s->cell[y], ' ', MU_COLS);
        memset(s->attr[y], 0, sizeof(s->attr[y]));
    }
    if (!side) {
        // straight under Umoria's last row: a divider would cost a line
        for (int i = 0; i < have; i++) {
            memcpy(s->cell[MU_GAME_ROWS + i], text[i], strlen(text[i]));
        }
        s->chat_dirty = 0;
        return;
    }

    uint32_t edge = mu_vt_border_colour > 0 ? MU_ATTR_FG(mu_vt_border_colour) : 0;
    memset(s->panel, ' ', sizeof(s->panel));
    memset(s->panel_attr, 0, sizeof(s->panel_attr));
    for (int x = 1; x < MU_PANEL_COLS - 1; x++) {
        panel_put(s, 0, x, MU_BOX_H, edge);
        panel_put(s, rows - 1, x, MU_BOX_H, edge);
    }
    for (int y = 1; y < rows - 1; y++) {
        panel_put(s, y, 0, MU_BOX_V, edge);
        panel_put(s, y, MU_PANEL_COLS - 1, MU_BOX_V, edge);
    }
    panel_put(s, 0, 0, MU_BOX_TL, edge);
    panel_put(s, 0, MU_PANEL_COLS - 1, MU_BOX_TR, edge);
    panel_put(s, rows - 1, 0, MU_BOX_BL, edge);
    panel_put(s, rows - 1, MU_PANEL_COLS - 1, MU_BOX_BR, edge);
    const char *title = " Chat ";
    for (int i = 0; title[i] != '\0'; i++) {
        panel_put(s, 0, 2 + i, title[i], edge | MU_ATTR_BOLD);
    }
    // the newest line at the bottom of the box, as in the pane
    for (int i = 0; i < have; i++) {
        int y = rows - 1 - have + i;
        memcpy(&s->panel[y][2], text[i], strlen(text[i]));
    }
    s->chat_dirty = 0;
}

// What a cell shows: the side panel; an animation over the map; else what was drawn there.
static char shown_ch(MuScreen *s, int y, int x) {
    if (x >= MU_COLS) {
        return s->panel[y][x - MU_COLS];
    }
    if (y < MU_GAME_ROWS && s->ov_ch[y][x] > MU_FX_TINT && s->mapcell[y][x]) {
        return s->ov_ch[y][x];
    }
    return s->cell[y][x];
}

static uint32_t shown_attr(MuScreen *s, int y, int x) {
    if (x >= MU_COLS) {
        return s->panel_attr[y][x - MU_COLS];
    }
    if (y < MU_GAME_ROWS && s->ov_ch[y][x] != 0 && s->mapcell[y][x]) {
        return s->ov_attr[y][x];
    }
    return s->attr[y][x];
}

static int same(MuScreen *s, int y, int x) {
    return shown_ch(s, y, x) == s->sent[y][x] && shown_attr(s, y, x) == s->sent_attr[y][x];
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

    int width = s->side ? MU_SCREEN_COLS : MU_COLS;
    int height = s->side ? s->rows_used : MU_ROWS;
    int style = border_of(s);
    for (int y = 0; y < height; y++) {
        int x = 0;
        while (x < width) {
            if (same(s, y, x)) {
                x++;
                continue;
            }
            int start = x;
            // join runs separated by a few equal cells: one jump, not several
            int end = x;
            while (end < width) {
                if (!same(s, y, end)) {
                    end++;
                    continue;
                }
                int gap = 0;
                while (end + gap < width && gap < 4 && same(s, y, end + gap)) {
                    gap++;
                }
                if (end + gap >= width || gap >= 4) {
                    break;
                }
                end += gap;
            }
            int n = snprintf(buf, sizeof(buf), "\033[%d;%dH", y + 1, start + 1);
            out_append(s, buf, n);
            for (int i = start; i < end; i++) {
                char ch = shown_ch(s, y, i);
                uint32_t look = shown_attr(s, y, i);
                set_look(s, look);
                if (ch >= MU_BOX_H && ch <= MU_BOX_BR) {
                    const char *piece = BOX[style][ch - MU_BOX_H]; // UTF-8: one column
                    out_append(s, piece, (int) strlen(piece));
                } else {
                    out_append(s, &ch, 1);
                }
                s->sent[y][i] = ch;
                s->sent_attr[y][i] = look;
            }
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

// Kept as said; wrapped (at a space, going on indented by two) when drawn.
void mu_vt_chat(MuScreen *s, const char *line) {
    pthread_mutex_lock(&s->lock);
    char *row = s->history[s->history_n % MU_CHAT_KEEP];
    snprintf(row, MU_CHAT_LINE, "%s", line);
    for (char *p = row; *p != '\0'; p++) {
        if ((unsigned char) *p < 32 || (unsigned char) *p > 126) {
            *p = ' ';
        }
    }
    s->history_n++;
    s->chat_dirty = 1;
    flush_locked(s);
    pthread_mutex_unlock(&s->lock);
    wake(s);
}

void mu_vt_set_size(MuScreen *s, int cols, int rows) {
    pthread_mutex_lock(&s->lock);
    if (cols != s->term_cols || rows != s->term_rows) {
        s->term_cols = cols;
        s->term_rows = rows;
        s->chat_dirty = 1;
        flush_locked(s);
    }
    pthread_mutex_unlock(&s->lock);
    wake(s);
}

void mu_vt_set_panel(MuScreen *s, int layout, int border) {
    pthread_mutex_lock(&s->lock);
    s->layout = layout;
    s->border = border;
    s->chat_dirty = 1;
    s->full_redraw = 1; // the outline's pieces are sent as they look
    flush_locked(s);
    pthread_mutex_unlock(&s->lock);
    wake(s);
}

int mu_vt_side(MuScreen *s) {
    pthread_mutex_lock(&s->lock);
    int side = want_side(s);
    pthread_mutex_unlock(&s->lock);
    return side;
}

int mu_vt_chat_width(MuScreen *s) {
    return mu_vt_side(s) ? SIDE_TEXT : MU_COLS;
}

// --- animations ----------------------------------------------------------------

int64_t mu_vt_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t) t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void mu_vt_map_drawing(MuScreen *s, int delta) {
    s->map_drawing += delta; // only the engine's thread draws the map
}

void mu_vt_map_panel(MuScreen *s, int top, int bottom, int left, int right, int row_prt, int col_prt) {
    pthread_mutex_lock(&s->lock);
    if (!s->map_set || s->map_top != top || s->map_left != left || s->map_row_prt != row_prt || s->map_col_prt != col_prt) {
        memset(s->ov_ch, 0, sizeof(s->ov_ch)); // drawn again at the next frame, in the new place
    }
    s->map_set = 1;
    s->map_top = top;
    s->map_bottom = bottom;
    s->map_left = left;
    s->map_right = right;
    s->map_row_prt = row_prt;
    s->map_col_prt = col_prt;
    pthread_mutex_unlock(&s->lock);
}

void mu_vt_fx_add(MuScreen *s, const MuFxCell *cells, int n) {
    if (n <= 0) {
        return;
    }
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < n && s->fx_n < MU_FX_MAX; i++) {
        s->fx[s->fx_n++] = cells[i];
    }
    pthread_mutex_unlock(&s->lock);
    wake(s);
}

void mu_vt_sound(MuScreen *s, int64_t t, const char *name, int volume, int pitch) {
    if (!s->sound) {
        return;
    }
    pthread_mutex_lock(&s->lock);
    if (s->sound_n < MU_SOUND_MAX) {
        MuSound *k = &s->sounds[s->sound_n++];
        k->t = t;
        snprintf(k->name, sizeof(k->name), "%s", name);
        k->volume = volume < 1 ? 1 : volume > 100 ? 100 : volume;
        k->pitch = pitch < 50 ? 50 : pitch > 200 ? 200 : pitch;
    }
    pthread_mutex_unlock(&s->lock);
    wake(s);
}

// The sounds due now, to the client; the time of the next one, -1 if none.
static int64_t send_sounds(MuScreen *s, int64_t now) {
    int64_t next = -1;
    int kept = 0;
    for (int i = 0; i < s->sound_n; i++) {
        MuSound k = s->sounds[i];
        if (k.t > now) {
            s->sounds[kept++] = k;
            next = next < 0 || k.t < next ? k.t : next;
            continue;
        }
        char msg[MU_SOUND_NAME + 24];
        int n = k.pitch == 100 ? snprintf(msg, sizeof(msg), "\377\372%c%s %d\377\360", MU_TELOPT_SOUND, k.name, k.volume)
                               : snprintf(msg, sizeof(msg), "\377\372%c%s %d %d\377\360", MU_TELOPT_SOUND, k.name, k.volume, k.pitch);
        out_append(s, msg, n);
    }
    s->sound_n = kept;
    return next;
}

void mu_vt_fx_clear(MuScreen *s) {
    pthread_mutex_lock(&s->lock);
    s->sound_n = 0;
    s->fx_n = 0;
    memset(s->ov_ch, 0, sizeof(s->ov_ch));
    s->map_set = 0;
    pthread_mutex_unlock(&s->lock);
}

int mu_vt_fx_tick(MuScreen *s) {
    static __thread char ch[MU_GAME_ROWS][MU_COLS];
    static __thread uint32_t look[MU_GAME_ROWS][MU_COLS];

    pthread_mutex_lock(&s->lock);
    int64_t now = mu_vt_now_ms();
    int64_t next = -1;
    memset(ch, 0, sizeof(ch));
    memset(look, 0, sizeof(look));
    int kept = 0;
    for (int i = 0; i < s->fx_n; i++) {
        MuFxCell c = s->fx[i];
        if (c.t1 <= now) {
            continue;
        }
        s->fx[kept++] = c;
        int64_t change = c.t0 > now ? c.t0 : c.t1;
        if (next < 0 || change < next) {
            next = change;
        }
        if (c.t0 > now || !s->map_set || c.y < s->map_top || c.y > s->map_bottom || c.x < s->map_left || c.x > s->map_right) {
            continue;
        }
        int y = c.y - s->map_row_prt, x = c.x - s->map_col_prt;
        if (y >= 1 && y < MU_GAME_ROWS && x >= 0 && x < MU_COLS) {
            ch[y][x] = c.ch; // a later cell is drawn over an earlier one
            look[y][x] = c.attr;
        }
    }
    s->fx_n = kept;
    int64_t sound_next = send_sounds(s, now);
    if (sound_next >= 0 && (next < 0 || sound_next < next)) {
        next = sound_next;
    }
    if (memcmp(ch, s->ov_ch, sizeof(ch)) != 0 || memcmp(look, s->ov_attr, sizeof(look)) != 0) {
        memcpy(s->ov_ch, ch, sizeof(ch));
        memcpy(s->ov_attr, look, sizeof(look));
        flush_locked(s);
    }
    pthread_mutex_unlock(&s->lock);
    return next < 0 ? -1 : (int) (next - now);
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
