// Mumoria - a small terminal client, for Linux, WSL, macOS and Windows.
//
// Any telnet client plays Mumoria; this one only saves setting up a terminal:
// it puts the terminal in raw mode, connects, and passes bytes both ways,
// dropping the server's telnet negotiation, except that it tells the server
// its window's size (NAWS), so a wide window gets the chat beside the map,
// and plays the game's sounds. Ctrl-] quits.
//
//     mumoria-client [-q] [host] [port]
//
// Sounds: the server sends IAC SB 212 "name volume" IAC SE (src/mumoria/vterm.h)
// once this client has answered its DO 212 with WILL; -q, or MUMORIA_SOUND=off,
// answers WONT. Each is sounds/<name>.wav beside this program; how a platform
// plays them is in its own file (client_posix.c, client_win.c). A missing
// file is silence. See client/sounds/README.md.
//
// What goes wrong (no server, a lost connection, no sound player, a crash) is
// told on leaving and kept in mumoria-client.log beside the program.

#include "client.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int client_sound = 1;
char client_dir[1024] = ".";

static int naws = 0;        // the server asked for the window's size
static int logged = 0;      // lines written to the log this time
static char log_path[1100]; // where the log is

// --- the log -------------------------------------------------------------------

const char *client_log_path(void) {
    return log_path;
}

void client_log(const char *fmt, ...) {
    if (log_path[0] == '\0') {
        snprintf(log_path, sizeof(log_path), "%s/mumoria-client.log", client_dir);
    }
    FILE *f = fopen(log_path, "a");
    if (f == NULL) {
        snprintf(log_path, sizeof(log_path), "mumoria-client.log"); // the program's folder may be read-only
        f = fopen(log_path, "a");
    }
    if (f == NULL) {
        return;
    }
    char when[32];
    time_t now = time(NULL);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(f, "%s ", when);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
    logged++;
}

// --- telnet ----------------------------------------------------------------------

// IAC SB NAWS width height IAC SE, a 255 doubled
static void send_size(void) {
    int cols = 0, rows = 0;
    if (!plat_term_size(&cols, &rows)) {
        return;
    }
    unsigned char msg[24];
    int n = 0;
    msg[n++] = 255;
    msg[n++] = 250;
    msg[n++] = TELOPT_NAWS;
    unsigned int v[4] = {(unsigned) cols >> 8, (unsigned) cols & 0xff, (unsigned) rows >> 8, (unsigned) rows & 0xff};
    for (int i = 0; i < 4; i++) {
        msg[n++] = (unsigned char) v[i];
        if (v[i] == 255) {
            msg[n++] = 255;
        }
    }
    msg[n++] = 255;
    msg[n++] = 240;
    (void) plat_send(msg, n);
}

void client_resized(void) {
    if (naws) {
        send_size();
    }
}

static long long now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// "name volume". The name comes from the server: only [a-z0-9_] is taken.
static void sound_message(const unsigned char *sb, int len) {
    static char last[32];
    static long long last_at;
    char text[64];
    int n = len - 1 < (int) sizeof(text) - 1 ? len - 1 : (int) sizeof(text) - 1;
    if (n <= 0) {
        return;
    }
    memcpy(text, sb + 1, (size_t) n);
    text[n] = '\0';
    char *space = strchr(text, ' ');
    int volume = 100;
    if (space != NULL) {
        *space = '\0';
        volume = atoi(space + 1);
    }
    if (text[0] == '\0' || strlen(text) >= sizeof(last)) {
        return;
    }
    for (const char *p = text; *p != '\0'; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) {
            return;
        }
    }
    long long t = now_ms();
    if (strcmp(text, last) == 0 && t - last_at < 60) {
        return; // the same sound twice at once is one sound
    }
    snprintf(last, sizeof(last), "%.31s", text); // shorter than last: checked above
    last_at = t;
    plat_sound_play(text, volume < 1 ? 1 : volume > 100 ? 100 : volume);
}

void client_feed(const unsigned char *buf, int n) {
    static int iac = 0; // 1 after IAC, 2 inside an option, 3 in a subnegotiation, 4 IAC in it
    static int verb = 0;
    static unsigned char sb[64]; // the subnegotiation being read
    static int sb_len = 0;
    unsigned char out[4096];
    int m = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = buf[i];
        switch (iac) {
            case 0:
                if (c == 255) {
                    iac = 1;
                } else {
                    out[m++] = c;
                }
                break;
            case 1:
                iac = (c == 250) ? 3 : (c >= 251 && c <= 254) ? 2 : 0;
                verb = c;
                sb_len = 0;
                if (c == 255) {
                    out[m++] = c;
                }
                break;
            case 2:
                iac = 0;
                if (verb == 253 && c == TELOPT_NAWS && !naws) { // DO NAWS: WILL, and the size
                    unsigned char will[3] = {255, 251, TELOPT_NAWS};
                    (void) plat_send(will, 3);
                    naws = 1;
                    send_size();
                }
                if (verb == 253 && c == TELOPT_SOUND) { // DO SOUND: WILL, unless -q
                    unsigned char reply[3] = {255, (unsigned char) (client_sound ? 251 : 252), TELOPT_SOUND};
                    (void) plat_send(reply, 3);
                }
                break;
            case 3:
                if (c == 255) {
                    iac = 4;
                } else if (sb_len < (int) sizeof(sb)) {
                    sb[sb_len++] = c;
                }
                break;
            default:
                if (c == 255 && sb_len < (int) sizeof(sb)) {
                    sb[sb_len++] = c; // a doubled 255
                    iac = 3;
                    break;
                }
                iac = (c == 240) ? 0 : 3;
                if (c == 240 && sb_len > 0 && sb[0] == TELOPT_SOUND && client_sound) {
                    sound_message(sb, sb_len);
                }
                break;
        }
        if (m == (int) sizeof(out)) {
            plat_out(out, m);
            m = 0;
        }
    }
    if (m > 0) {
        plat_out(out, m);
    }
}

// --- main ------------------------------------------------------------------------

int main(int argc, char *argv[]) {
    const char *sound_env = getenv("MUMORIA_SOUND");
    client_sound = sound_env == NULL || strcmp(sound_env, "off") != 0;
    int arg = 1;
    if (arg < argc && (strcmp(argv[arg], "-h") == 0 || strcmp(argv[arg], "--help") == 0)) {
        printf("Usage: mumoria-client [-q] [host] [port]   (-q: no sounds; Ctrl-] quits)\n");
        return 0;
    }
    if (arg < argc && strcmp(argv[arg], "-q") == 0) {
        client_sound = 0;
        arg++;
    }
    const char *host = argc > arg ? argv[arg] : "localhost";
    const char *port = argc > arg + 1 ? argv[arg + 1] : "4000";

    char why[512];
    if (plat_start(argv[0], why, sizeof(why)) != 0) {
        fprintf(stderr, "mumoria-client: %s\n", why);
        client_log("cannot start: %s", why);
        return 1;
    }
    plat_catch_crashes();
    if (client_sound) {
        plat_sound_start();
    }
    if (plat_dial(host, port, why, sizeof(why)) != 0) {
        fprintf(stderr, "Cannot reach %s:%s: %s\n", host, port, why);
        client_log("cannot reach %s:%s: %s", host, port, why);
        return 1;
    }

    plat_raw();
    int failed = 0;
    const char *ended = plat_run(&failed);
    plat_restore();
    if (ended != NULL) {
        fprintf(stderr, "\n%s\n", ended);
        if (failed) {
            client_log("%s (%s:%s)", ended, host, port);
        }
    }
    if (logged > 0) {
        fprintf(stderr, "Problems were noted in %s\n", client_log_path());
    }
    return failed ? 1 : 0;
}
