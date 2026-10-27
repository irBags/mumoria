// Mumoria client - what the shared part (mumoria_client.c) and each
// platform's part (client_posix.c: Linux, WSL, macOS; client_win.c: Windows)
// give each other.

#pragma once

#define CONNECT_SECONDS 10 // how long to wait for the server to answer
#define TELOPT_NAWS 31
#define TELOPT_SOUND 212 // the server's sounds (src/mumoria/vterm.h)

// --- mumoria_client.c ----------------------------------------------------------

extern int client_sound;     // play sounds (no -q, MUMORIA_SOUND not off)
extern char client_dir[1024]; // the folder this program is in (sounds/, the log)

// A line in mumoria-client.log (beside the program, else in the current
// folder), with the date and time. For what went wrong, not for play.
void client_log(const char *fmt, ...);
// The log's path, for telling the player where to look.
const char *client_log_path(void);
// Bytes from the server: the screen goes out, telnet is answered, sounds play.
void client_feed(const unsigned char *buf, int n);
// The terminal's size changed: tell the server, if it asked (NAWS).
void client_resized(void);

// --- the platform ----------------------------------------------------------------

// Sockets and the like; the folder the program is in, into client_dir.
// 0 if all is well, else why not into why.
int plat_start(const char *argv0, char *why, int why_len);
// Connects; 0, else why not into why.
int plat_dial(const char *host, const char *port, char *why, int why_len);
// To the server; false if the connection is gone.
int plat_send(const void *buf, int n);
// To the terminal.
void plat_out(const void *buf, int n);
// The terminal's columns and rows; 0 if not known.
int plat_term_size(int *cols, int *rows);
// Raw keys in, colours out; put back by plat_restore (also on a crash).
void plat_raw(void);
void plat_restore(void);
// Passes keys to the server and the server's bytes to client_feed until the
// connection ends or the player quits (Ctrl-]). Returns why it ended (null
// for a quit); *failed is set when that was a fault (a lost connection), not
// the server saying goodbye.
const char *plat_run(int *failed);
// Sounds: get ready (sounds/ beside the program); play one by name (only
// [a-z0-9_]), volume 1-100, pitch a percentage (100: as recorded). Where
// there are name2.wav ... name9.wav too, one of them at random. Returns 0
// (and plays nothing) if there is no such sound.
void plat_sound_start(void);
int plat_sound_play(const char *name, int volume, int pitch);
// A crash: the terminal put back, where it happened in the log, then gone.
void plat_catch_crashes(void);
