// Mumoria client - Windows (client.h). Built by build.sh (zig cc).
//
// The console is put in virtual-terminal mode both ways, so the game's
// colours show and the arrow keys arrive as ANSI, as on Linux. Keys are read
// by a thread; the server's bytes by the main one.
//
// Sounds: every sounds\*.wav is read at the start, and a mixer thread keeps
// one waveOut stream going (silence between sounds), so sounds overlap, each
// at its own volume, and none waits for the sound card to wake. Plain PCM
// WAVs, 16-bit, mono or stereo, any rate.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>

#include "client.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SOCKET sock = INVALID_SOCKET;
static HANDLE in_h, out_h;
static DWORD in_mode, out_mode;
static UINT old_cp_in, old_cp_out;
static int raw_on = 0;
static volatile LONG quitting = 0, input_closed = 0;

// The system's words for an error code.
static void win_error(DWORD code, char *buf, int len) {
    int n = (int) FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, buf, (DWORD) len, NULL);
    while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == '.' || buf[n - 1] == ' ')) {
        buf[--n] = '\0';
    }
    if (n == 0) {
        snprintf(buf, (size_t) len, "error %lu", (unsigned long) code);
    }
}

int plat_start(const char *argv0, char *why, int why_len) {
    (void) argv0;
    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        char msg[200];
        win_error((DWORD) rc, msg, sizeof(msg));
        snprintf(why, (size_t) why_len, "no networking (WSAStartup: %s)", msg);
        return -1;
    }
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (n > 0 && n < sizeof(exe)) {
        char *slash = strrchr(exe, '\\');
        if (slash != NULL) {
            *slash = '\0';
            snprintf(client_dir, sizeof(client_dir), "%s", exe);
        }
    }
    in_h = GetStdHandle(STD_INPUT_HANDLE);
    out_h = GetStdHandle(STD_OUTPUT_HANDLE);
    return 0;
}

int plat_dial(const char *host, const char *port, char *why, int why_len) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        win_error((DWORD) rc, why, why_len);
        return -1;
    }
    snprintf(why, (size_t) why_len, "no address to try");
    for (struct addrinfo *a = res; a != NULL; a = a->ai_next) {
        SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) {
            win_error((DWORD) WSAGetLastError(), why, why_len);
            continue;
        }
        // at most CONNECT_SECONDS: a server that never answers is told, not waited on
        u_long on = 1, off = 0;
        ioctlsocket(s, FIONBIO, &on);
        int ok = connect(s, a->ai_addr, (int) a->ai_addrlen) == 0;
        DWORD err = ok ? 0 : (DWORD) WSAGetLastError();
        if (!ok && err == WSAEWOULDBLOCK) {
            fd_set w, x;
            FD_ZERO(&w);
            FD_ZERO(&x);
            FD_SET(s, &w);
            FD_SET(s, &x);
            struct timeval tv = {CONNECT_SECONDS, 0};
            int ready = select(0, NULL, &w, &x, &tv);
            int so_err = 0, len = sizeof(so_err);
            if (ready <= 0) {
                err = WSAETIMEDOUT;
            } else if (FD_ISSET(s, &w)) {
                ok = 1;
            } else {
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char *) &so_err, &len);
                err = (DWORD) so_err;
            }
        }
        if (ok) {
            ioctlsocket(s, FIONBIO, &off);
            sock = s;
            break;
        }
        if (err == WSAETIMEDOUT) {
            snprintf(why, (size_t) why_len, "no answer (timed out)");
        } else {
            win_error(err, why, why_len);
        }
        closesocket(s);
    }
    freeaddrinfo(res);
    return sock == INVALID_SOCKET ? -1 : 0;
}

int plat_send(const void *buf, int n) {
    return send(sock, (const char *) buf, n, 0) == n;
}

// A UTF-8 character split between two reads is held back until it is whole,
// or the console would draw it as two wrong ones.
void plat_out(const void *buf, int n) {
    static unsigned char carry[4];
    static int carry_n = 0;
    unsigned char data[4100];
    int len = 0;
    memcpy(data, carry, (size_t) carry_n);
    len = carry_n;
    while (n > 0) {
        int take = n < (int) sizeof(data) - len ? n : (int) sizeof(data) - len;
        memcpy(data + len, buf, (size_t) take);
        len += take;
        buf = (const unsigned char *) buf + take;
        n -= take;
        int whole = len;
        for (int back = 1; back <= 3 && back <= len; back++) {
            unsigned char c = data[len - back];
            if ((c & 0xC0) == 0x80) {
                continue; // a continuation byte: look further back
            }
            int need = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
            if (need > back) {
                whole = len - back; // its last bytes have not come yet
            }
            break;
        }
        DWORD wrote;
        WriteFile(out_h, data, (DWORD) whole, &wrote, NULL);
        carry_n = len - whole;
        memcpy(carry, data + whole, (size_t) carry_n);
        len = 0;
        if (n > 0) {
            memcpy(data, carry, (size_t) carry_n);
            len = carry_n;
        }
    }
}

int plat_term_size(int *cols, int *rows) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(out_h, &info)) {
        return 0;
    }
    *cols = info.srWindow.Right - info.srWindow.Left + 1;
    *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    return 1;
}

void plat_restore(void) {
    if (!raw_on) {
        return;
    }
    raw_on = 0;
    DWORD wrote;
    WriteFile(out_h, "\033[0m\r\n", 6, &wrote, NULL);
    SetConsoleMode(in_h, in_mode);
    SetConsoleMode(out_h, out_mode);
    SetConsoleCP(old_cp_in);
    SetConsoleOutputCP(old_cp_out);
}

// The window closed, or Ctrl-Break: the console back as it was.
static BOOL WINAPI on_console_event(DWORD event) {
    (void) event;
    plat_restore();
    return FALSE;
}

void plat_raw(void) {
    old_cp_in = GetConsoleCP();
    old_cp_out = GetConsoleOutputCP();
    int in_console = GetConsoleMode(in_h, &in_mode);
    int out_console = GetConsoleMode(out_h, &out_mode);
    if (!in_console && !out_console) {
        return; // not a console (a test): nothing to set
    }
    raw_on = 1;
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    // keys as typed, Ctrl-C too, arrows as ANSI
    if (in_console && !SetConsoleMode(in_h, ENABLE_VIRTUAL_TERMINAL_INPUT)) {
        client_log("this console cannot send arrow keys as the game wants them (use Windows Terminal, or the number keys)");
        SetConsoleMode(in_h, 0);
    }
    if (out_console && !SetConsoleMode(out_h, out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        client_log("this console does not understand the game's colours and cursor moves: use Windows Terminal "
                   "(or Windows 10 1809 or later)");
    }
    SetConsoleCtrlHandler(on_console_event, TRUE);
    atexit(plat_restore);
}

static DWORD WINAPI read_keys(LPVOID arg) {
    (void) arg;
    unsigned char buf[512];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(in_h, buf, sizeof(buf), &n, NULL)) {
            DWORD err = GetLastError();
            if (err == ERROR_NO_DATA) {
                Sleep(20); // a non-blocking pipe with nothing in it yet
                continue;
            }
            if (err != ERROR_BROKEN_PIPE && err != ERROR_HANDLE_EOF) {
                char msg[200];
                win_error(err, msg, sizeof(msg));
                client_log("cannot read the keyboard: %s (%lu)", msg, (unsigned long) err);
            }
            InterlockedExchange(&input_closed, 1);
            break;
        }
        if (n == 0) {
            InterlockedExchange(&input_closed, 1);
            break;
        }
        if (memchr(buf, 29, n) != NULL) { // Ctrl-]
            InterlockedExchange(&quitting, 1);
            break;
        }
        if (!plat_send(buf, (int) n)) {
            break;
        }
    }
    shutdown(sock, SD_BOTH); // wakes the main thread
    return 0;
}

const char *plat_run(int *failed) {
    static char why[300];
    HANDLE keys = CreateThread(NULL, 0, read_keys, NULL, 0, NULL);
    if (keys == NULL) {
        char msg[200];
        win_error(GetLastError(), msg, sizeof(msg));
        snprintf(why, sizeof(why), "Cannot read the keyboard: %s", msg);
        *failed = 1;
        return why;
    }
    int cols = 0, rows = 0;
    plat_term_size(&cols, &rows);
    char buf[4096];
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        struct timeval tv = {0, 250000};
        int ready = select(0, &fds, NULL, NULL, &tv);
        if (ready == 0) {
            int c = 0, r = 0;
            if (plat_term_size(&c, &r) && (c != cols || r != rows)) {
                cols = c;
                rows = r;
                client_resized();
            }
            continue;
        }
        int n = ready < 0 ? -1 : recv(sock, buf, sizeof(buf), 0);
        if (n > 0) {
            client_feed((const unsigned char *) buf, n);
            continue;
        }
        if (quitting) {
            return NULL;
        }
        if (input_closed) {
            return "The keyboard input closed.";
        }
        if (n == 0) {
            return "The server closed the connection.";
        }
        char msg[200];
        win_error((DWORD) WSAGetLastError(), msg, sizeof(msg));
        snprintf(why, sizeof(why), "Lost the connection: %s", msg);
        *failed = 1;
        return why;
    }
}

// --- sounds ----------------------------------------------------------------------

#define RATE 22050
#define FRAMES 512 // a buffer: 23 ms
#define NBUF 4
#define VOICES 16
#define CLIPS 512

typedef struct {
    char name[32];
    short *pcm;
    int len;
} Clip;

// pos and step in 1/65536ths of a frame: a step over 65536 plays it higher.
typedef struct {
    const short *pcm;
    long long len, pos;
    int step, gain; // gain 0-256
} Voice;

static char sound_dir[1100];
static Clip clips[CLIPS];
static int clip_n = 0;
static Voice voices[VOICES];
static CRITICAL_SECTION mix_lock;
static HWAVEOUT wave;
static HANDLE wave_event;
static WAVEHDR headers[NBUF];
static short buffers[NBUF][FRAMES];

static unsigned rd16(const unsigned char *p) {
    return (unsigned) p[0] | (unsigned) p[1] << 8;
}

static unsigned rd32(const unsigned char *p) {
    return rd16(p) | rd16(p + 2) << 16;
}

// A PCM WAV as mono 16-bit at RATE; null (and why) if it cannot be.
static short *load_wav(const char *path, int *frames, char *why, int why_len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        snprintf(why, (size_t) why_len, "cannot open it");
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *file = size > 44 && size < 16 * 1024 * 1024 ? malloc((size_t) size) : NULL;
    if (file == NULL || fread(file, 1, (size_t) size, f) != (size_t) size) {
        fclose(f);
        free(file);
        snprintf(why, (size_t) why_len, "cannot read it");
        return NULL;
    }
    fclose(f);
    short *out = NULL;
    unsigned channels = 0, rate = 0, bits = 0, format = 0;
    const unsigned char *data = NULL;
    unsigned data_len = 0;
    if (memcmp(file, "RIFF", 4) == 0 && memcmp(file + 8, "WAVE", 4) == 0) {
        long at = 12;
        while (at + 8 <= size) {
            unsigned len = rd32(file + at + 4);
            if (len > (unsigned long) (size - at - 8)) {
                len = (unsigned) (size - at - 8);
            }
            if (memcmp(file + at, "fmt ", 4) == 0 && len >= 16) {
                format = rd16(file + at + 8);
                channels = rd16(file + at + 10);
                rate = rd32(file + at + 12);
                bits = rd16(file + at + 22);
            } else if (memcmp(file + at, "data", 4) == 0) {
                data = file + at + 8;
                data_len = len;
            }
            at += 8 + len + (len & 1);
        }
    }
    if (format != 1 || bits != 16 || (channels != 1 && channels != 2) || rate < 4000 || data == NULL) {
        snprintf(why, (size_t) why_len, "not a 16-bit PCM WAV (format %u, %u bits, %u channels)", format, bits, channels);
        free(file);
        return NULL;
    }
    int in_frames = (int) (data_len / (2 * channels));
    int n = (int) ((long long) in_frames * RATE / rate);
    out = n > 0 ? malloc(sizeof(short) * (size_t) n) : NULL;
    for (int i = 0; out != NULL && i < n; i++) {
        double t = (double) i * rate / RATE;
        int a = (int) t;
        int b = a + 1 < in_frames ? a + 1 : a;
        double frac = t - a;
        double va = 0, vb = 0;
        for (unsigned c = 0; c < channels; c++) {
            va += (short) rd16(data + 2 * (a * channels + c));
            vb += (short) rd16(data + 2 * (b * channels + c));
        }
        out[i] = (short) ((va + (vb - va) * frac) / channels);
    }
    free(file);
    *frames = n;
    return out;
}

static void load_sounds(void) {
    char pattern[1200];
    snprintf(pattern, sizeof(pattern), "%s\\*.wav", sound_dir);
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(pattern, &found);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        size_t len = strlen(found.cFileName);
        if (len < 5 || len - 4 >= sizeof(clips[0].name) || clip_n == CLIPS) {
            continue;
        }
        char path[1400], why[200];
        snprintf(path, sizeof(path), "%s\\%s", sound_dir, found.cFileName);
        Clip *c = &clips[clip_n];
        c->pcm = load_wav(path, &c->len, why, sizeof(why));
        if (c->pcm == NULL) {
            client_log("sound %s left out: %s", found.cFileName, why);
            continue;
        }
        snprintf(c->name, sizeof(c->name), "%.*s", (int) (len - 4), found.cFileName);
        clip_n++;
    } while (FindNextFileA(h, &found));
    FindClose(h);
}

static void fill(WAVEHDR *h) {
    int acc[FRAMES] = {0};
    EnterCriticalSection(&mix_lock);
    for (int v = 0; v < VOICES; v++) {
        Voice *s = &voices[v];
        for (int i = 0; s->pcm != NULL && i < FRAMES; i++) {
            if (s->pos >= s->len) {
                s->pcm = NULL; // done
                break;
            }
            acc[i] += s->pcm[s->pos >> 16] * s->gain / 256;
            s->pos += s->step;
        }
    }
    LeaveCriticalSection(&mix_lock);
    short *out = (short *) h->lpData;
    for (int i = 0; i < FRAMES; i++) {
        out[i] = (short) (acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
    }
}

static DWORD WINAPI mixer(LPVOID arg) {
    (void) arg;
    for (;;) {
        WaitForSingleObject(wave_event, INFINITE);
        for (int b = 0; b < NBUF; b++) {
            if (headers[b].dwFlags & WHDR_DONE) {
                fill(&headers[b]);
                waveOutWrite(wave, &headers[b], sizeof(WAVEHDR));
            }
        }
    }
    return 0;
}

void plat_sound_start(void) {
    srand((unsigned) GetTickCount()); // which of a sound's variants plays
    const char *env = getenv("MUMORIA_SOUNDS");
    if (env != NULL && *env != '\0') {
        snprintf(sound_dir, sizeof(sound_dir), "%s", env);
    } else {
        snprintf(sound_dir, sizeof(sound_dir), "%s\\sounds", client_dir);
    }
    load_sounds();
    if (clip_n == 0) {
        client_log("no sounds in %s: playing without them", sound_dir);
        client_sound = 0;
        return;
    }
    InitializeCriticalSection(&mix_lock);
    wave_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    WAVEFORMATEX fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = RATE * 2;
    MMRESULT rc = waveOutOpen(&wave, WAVE_MAPPER, &fmt, (DWORD_PTR) wave_event, 0, CALLBACK_EVENT);
    if (rc != MMSYSERR_NOERROR) {
        char msg[256] = "";
        waveOutGetErrorTextA(rc, msg, sizeof(msg));
        client_log("cannot play sounds: no sound device (%s)", msg);
        client_sound = 0;
        return;
    }
    for (int b = 0; b < NBUF; b++) {
        headers[b].lpData = (LPSTR) buffers[b];
        headers[b].dwBufferLength = sizeof(buffers[b]);
        waveOutPrepareHeader(wave, &headers[b], sizeof(WAVEHDR));
        fill(&headers[b]);
        waveOutWrite(wave, &headers[b], sizeof(WAVEHDR));
    }
    if (CreateThread(NULL, 0, mixer, NULL, 0, NULL) == NULL) {
        client_log("cannot play sounds: no mixer thread");
        client_sound = 0;
    }
}

int plat_sound_play(const char *name, int volume, int pitch) {
    Clip *c = NULL;
    int found = 0;
    size_t n = strlen(name);
    for (int i = 0; i < clip_n; i++) {
        const char *k = clips[i].name;
        int variant = strncmp(k, name, n) == 0 && (k[n] == '\0' || (k[n] >= '2' && k[n] <= '9' && k[n + 1] == '\0'));
        if (variant && rand() % ++found == 0) {
            c = &clips[i]; // each of them as likely
        }
    }
    if (c == NULL) {
        return 0;
    }
    EnterCriticalSection(&mix_lock);
    int slot = 0;
    for (int v = 0; v < VOICES; v++) {
        if (voices[v].pcm == NULL) {
            slot = v;
            break;
        }
        if (voices[v].pos > voices[slot].pos) {
            slot = v; // all busy: the one furthest on gives way
        }
    }
    voices[slot] = (Voice){c->pcm, (long long) c->len << 16, 0, pitch * 65536 / 100, volume * 256 / 100};
    LeaveCriticalSection(&mix_lock);
    return 1;
}

// --- crashes ---------------------------------------------------------------------

static void crashed(const char *what, void *where) {
    plat_restore();
    char line[300];
    char *base = (char *) GetModuleHandleA(NULL);
    snprintf(line, sizeof(line), "=== mumoria-client crashed: %s at %p (offset 0x%llx in the program) ===", what, where,
             (unsigned long long) ((char *) where - base));
    fprintf(stderr, "\n%s\n", line);
    client_log("%s", line);
    fprintf(stderr, "Written to %s\n", client_log_path());
}

static LONG WINAPI on_exception(EXCEPTION_POINTERS *e) {
    char what[64];
    snprintf(what, sizeof(what), "exception 0x%08lx", (unsigned long) e->ExceptionRecord->ExceptionCode);
    crashed(what, e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void on_abort(int sig) {
    (void) sig;
    crashed("abort", NULL);
    _exit(3);
}

void plat_catch_crashes(void) {
    SetUnhandledExceptionFilter(on_exception);
    signal(SIGABRT, on_abort);
}
