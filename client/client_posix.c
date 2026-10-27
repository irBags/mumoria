// Mumoria client - Linux, WSL and macOS (client.h).
//
// Sounds are played by a command, one process a sound: $MUMORIA_PLAYER, else
// paplay (Linux, WSL) or afplay (macOS). In the command %f is the file, %v
// the volume 0-100, %p it for paplay (0-65536), %a for afplay (0.00-1.00)
// and %r the rate to play it at for its pitch (the file's own at 100%).
// paplay takes a file's rate from the file, so a pitched sound goes to it
// as raw samples at the new rate instead; afplay plays every pitch as recorded.

#include "client.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#if defined(__GLIBC__) || defined(__APPLE__)
#include <execinfo.h>
#define HAVE_BACKTRACE 1
#endif

static int sock = -1;
static struct termios saved;
static int raw_on = 0;
static volatile sig_atomic_t resized = 0;

static void on_resize(int sig) {
    (void) sig;
    resized = 1;
}

int plat_start(const char *argv0, char *why, int why_len) {
    (void) why;
    (void) why_len;
    char exe[900] = "";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
    } else if (realpath(argv0, exe) == NULL) {
        snprintf(exe, sizeof(exe), "%s", argv0);
    }
    char *slash = strrchr(exe, '/');
    if (slash != NULL) {
        *slash = '\0';
        snprintf(client_dir, sizeof(client_dir), "%s", exe);
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN); // the sound players reap themselves
    signal(SIGWINCH, on_resize);
    return 0;
}

int plat_dial(const char *host, const char *port, char *why, int why_len) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        snprintf(why, (size_t) why_len, "%s", gai_strerror(rc));
        return -1;
    }
    snprintf(why, (size_t) why_len, "no address to try");
    for (struct addrinfo *a = res; a != NULL && sock < 0; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) {
            snprintf(why, (size_t) why_len, "%s", strerror(errno));
            continue;
        }
        // at most CONNECT_SECONDS: a server that never answers is told, not waited on for minutes
        int flags = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            fd_set w;
            FD_ZERO(&w);
            FD_SET(fd, &w);
            struct timeval tv = {CONNECT_SECONDS, 0};
            int err = 0;
            socklen_t len = sizeof(err);
            if (select(fd + 1, NULL, &w, NULL, &tv) <= 0) {
                errno = ETIMEDOUT;
            } else if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                rc = 0;
            } else {
                errno = err;
            }
        }
        if (rc == 0) {
            fcntl(fd, F_SETFL, flags);
            sock = fd;
            break;
        }
        snprintf(why, (size_t) why_len, "%s", errno == ETIMEDOUT ? "no answer (timed out)" : strerror(errno));
        close(fd);
    }
    freeaddrinfo(res);
    if (sock < 0) {
        return -1;
    }
    fcntl(sock, F_SETFD, FD_CLOEXEC); // not held open by a sound player
    return 0;
}

int plat_send(const void *buf, int n) {
    return send(sock, buf, (size_t) n, 0) == n;
}

void plat_out(const void *buf, int n) {
    (void) !write(STDOUT_FILENO, buf, (size_t) n);
}

int plat_term_size(int *cols, int *rows) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) != 0 || w.ws_col == 0) {
        return 0;
    }
    *cols = w.ws_col;
    *rows = w.ws_row;
    return 1;
}

void plat_raw(void) {
    if (tcgetattr(STDIN_FILENO, &saved) != 0) {
        return; // not a terminal (a test): nothing to set
    }
    struct termios raw = saved;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    raw_on = 1;
}

void plat_restore(void) {
    if (raw_on) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        raw_on = 0;
        printf("\033[0m\n");
        fflush(stdout);
    }
}

const char *plat_run(int *failed) {
    static char why[200];
    unsigned char buf[4096];
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        FD_SET(sock, &fds);
        if (select(sock + 1, &fds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) {
                if (resized) {
                    resized = 0;
                    client_resized();
                }
                continue;
            }
            snprintf(why, sizeof(why), "Stopped: %s", strerror(errno));
            *failed = 1;
            return why;
        }
        if (FD_ISSET(STDIN_FILENO, &fds)) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) {
                return "The keyboard input closed.";
            }
            if (memchr(buf, 29, (size_t) n) != NULL) { // Ctrl-]
                return NULL;
            }
            if (!plat_send(buf, (int) n)) {
                snprintf(why, sizeof(why), "Lost the connection: %s", strerror(errno));
                *failed = 1;
                return why;
            }
        }
        if (FD_ISSET(sock, &fds)) {
            ssize_t n = recv(sock, buf, sizeof(buf), 0);
            if (n == 0) {
                return "The server closed the connection.";
            }
            if (n < 0) {
                snprintf(why, sizeof(why), "Lost the connection: %s", strerror(errno));
                *failed = 1;
                return why;
            }
            client_feed(buf, (int) n);
        }
    }
}

// --- sounds ----------------------------------------------------------------------

static char sound_dir[1100];
static const char *player_cmd;

// Is the command's first word a program on the PATH (or a path that runs)?
static int runnable(const char *cmd) {
    char prog[256];
    int i = 0;
    while (cmd[i] != '\0' && cmd[i] != ' ' && i < (int) sizeof(prog) - 1) {
        prog[i] = cmd[i];
        i++;
    }
    prog[i] = '\0';
    if (strchr(prog, '/') != NULL) {
        return access(prog, X_OK) == 0;
    }
    const char *path = getenv("PATH");
    char dir[1024], full[1300];
    while (path != NULL && *path != '\0') {
        const char *colon = strchr(path, ':');
        int len = colon != NULL ? (int) (colon - path) : (int) strlen(path);
        snprintf(dir, sizeof(dir), "%.*s", len, path);
        snprintf(full, sizeof(full), "%s/%s", dir[0] != '\0' ? dir : ".", prog);
        if (access(full, X_OK) == 0) {
            return 1;
        }
        path = colon != NULL ? colon + 1 : NULL;
    }
    return 0;
}

// PulseAudio (WSLg too) suspends the sound card after a few idle seconds,
// and the next sound waits while it wakes: a door heard after you went
// through it. A silent stream held open keeps it awake. Not on macOS;
// MUMORIA_KEEPALIVE=off leaves it out.
static pid_t keepalive = 0;

static void stop_keepalive(void) {
    if (keepalive > 0) {
        kill(keepalive, SIGTERM);
        keepalive = 0;
    }
}

static void start_keepalive(void) {
#ifndef __APPLE__
    const char *env = getenv("MUMORIA_KEEPALIVE");
    if ((env != NULL && strcmp(env, "off") == 0) || !runnable("pacat")) {
        return;
    }
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGTERM); // gone with the client, however it ends
        if (getppid() != parent) {
            _exit(0);
        }
#endif
        int zero = open("/dev/zero", O_RDONLY), null = open("/dev/null", O_WRONLY);
        if (zero >= 0 && null >= 0) {
            dup2(zero, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
        }
        execlp("pacat", "pacat", "--playback", "--raw", "--format=s16le", "--rate=8000", "--channels=1",
               "--client-name=mumoria-keepalive", (char *) NULL);
        _exit(127);
    }
    if (pid > 0) {
        keepalive = pid;
        atexit(stop_keepalive);
    }
#endif
}

// The sounds there are (their names, without .wav), read at the start.
#define KNOWN 512
static char known[KNOWN][32];
static int known_n = 0;

// Reads each sound once, so its first play is not slowed by the disk
// (under WSL, /mnt/c is slow the first time). Returns how many there are.
static int warm_sounds(void) {
    DIR *d = opendir(sound_dir);
    if (d == NULL) {
        return 0;
    }
    int count = 0;
    struct dirent *e;
    char path[1400], buf[65536];
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".wav") != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", sound_dir, e->d_name);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            while (read(fd, buf, sizeof(buf)) > 0) {
            }
            close(fd);
            count++;
            if (known_n < KNOWN && n - 4 < sizeof(known[0])) {
                snprintf(known[known_n++], sizeof(known[0]), "%.*s", (int) (n - 4), e->d_name);
            }
        }
    }
    closedir(d);
    return count;
}

void plat_sound_start(void) {
    srand((unsigned) getpid()); // which of a sound's variants plays
    const char *env = getenv("MUMORIA_SOUNDS");
    snprintf(sound_dir, sizeof(sound_dir), "%s", env != NULL && *env != '\0' ? env : client_dir);
    if (env == NULL || *env == '\0') {
        strncat(sound_dir, "/sounds", sizeof(sound_dir) - strlen(sound_dir) - 1);
    }
    player_cmd = getenv("MUMORIA_PLAYER");
    if (player_cmd == NULL || *player_cmd == '\0') {
#ifdef __APPLE__
        player_cmd = "afplay -v %a %f";
#else
        player_cmd = "paplay --volume=%p %f";
#endif
    }
    if (warm_sounds() == 0) {
        client_log("no sounds in %s: playing without them", sound_dir);
        client_sound = 0;
        return;
    }
    if (strchr(sound_dir, '\'') != NULL) {
        client_log("the sounds folder's name has a ' in it (%s): playing without sounds", sound_dir);
        client_sound = 0;
        return;
    }
    if (!runnable(player_cmd)) {
        client_log("cannot play sounds: \"%s\" is not installed (on Linux: the pulseaudio-utils package), "
                   "or set MUMORIA_PLAYER", player_cmd);
        client_sound = 0;
        return;
    }
    start_keepalive();
}

// A 16-bit PCM WAV's rate, channels and where its samples start; 0 if it is not one.
static int wav_facts(const char *file, int *rate, int *channels, long *data) {
    unsigned char h[512];
    int fd = open(file, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    long n = (long) read(fd, h, sizeof(h));
    close(fd);
    if (n < 44 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) {
        return 0;
    }
    int bits = 0;
    *rate = 0;
    for (long at = 12; at + 8 <= n;) {
        long len = (long) (h[at + 4] | h[at + 5] << 8 | h[at + 6] << 16 | (unsigned long) h[at + 7] << 24);
        if (memcmp(h + at, "fmt ", 4) == 0 && at + 24 <= n) {
            *channels = h[at + 10] | h[at + 11] << 8;
            *rate = h[at + 12] | h[at + 13] << 8 | h[at + 14] << 16;
            bits = h[at + 22] | h[at + 23] << 8;
        } else if (memcmp(h + at, "data", 4) == 0) {
            *data = at + 8;
            return bits == 16 && *rate > 0;
        }
        at += 8 + len + (len & 1);
    }
    return 0;
}

int plat_sound_play(const char *name, int volume, int pitch) {
    const char *pick = NULL;
    int found = 0;
    size_t len = strlen(name);
    for (int i = 0; i < known_n; i++) { // name.wav, name2.wav ... name9.wav, each as likely
        const char *k = known[i];
        int variant = strncmp(k, name, len) == 0 && (k[len] == '\0' || (k[len] >= '2' && k[len] <= '9' && k[len + 1] == '\0'));
        if (variant && rand() % ++found == 0) {
            pick = k;
        }
    }
    if (pick == NULL) {
        return 0;
    }
    char file[1300];
    snprintf(file, sizeof(file), "%s/%s.wav", sound_dir, pick);
    int rate = 22050, channels = 1;
    long data = 44;
    int wav = wav_facts(file, &rate, &channels, &data);
    int play_rate = rate * pitch / 100;
    char cmd[2048];
    int m = 0;
    const char *how = player_cmd;
    char raw[256];
    if (pitch != 100 && wav && strcmp(player_cmd, "paplay --volume=%p %f") == 0) {
        snprintf(raw, sizeof(raw), "tail -c +%ld %%f | paplay --raw --format=s16le --channels=%d --rate=%%r --volume=%%p", data + 1, channels);
        how = raw;
    }
    for (const char *p = how; *p != '\0' && m < (int) sizeof(cmd) - 1400; p++) {
        if (*p != '%' || p[1] == '\0') {
            cmd[m++] = *p;
            continue;
        }
        p++;
        switch (*p) {
            case 'f': m += snprintf(cmd + m, sizeof(cmd) - (size_t) m, "'%s'", file); break;
            case 'v': m += snprintf(cmd + m, sizeof(cmd) - (size_t) m, "%d", volume); break;
            case 'p': m += snprintf(cmd + m, sizeof(cmd) - (size_t) m, "%d", volume * 65536 / 100); break;
            case 'a': m += snprintf(cmd + m, sizeof(cmd) - (size_t) m, "%d.%02d", volume / 100, volume % 100); break;
            case 'r': m += snprintf(cmd + m, sizeof(cmd) - (size_t) m, "%d", play_rate); break;
            default: cmd[m++] = *p; break;
        }
    }
    cmd[m] = '\0';
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *) NULL);
        _exit(127);
    }
    return 1;
}

// --- crashes ---------------------------------------------------------------------

static char crash_log[1100];

static void say(int fd, const char *s) {
    (void) !write(fd, s, strlen(s));
}

static void on_crash(int sig) {
    if (raw_on) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved); // the player's terminal back first
    }
    int log = open(crash_log, O_WRONLY | O_CREAT | O_APPEND, 0644);
    int fds[2] = {STDERR_FILENO, log};
    for (int i = 0; i < 2; i++) {
        if (fds[i] < 0) {
            continue;
        }
        say(fds[i], "\r\n=== mumoria-client crashed: ");
        say(fds[i], strsignal(sig));
        say(fds[i], " ===\n");
#ifdef HAVE_BACKTRACE
        void *frames[48];
        int n = backtrace(frames, 48);
        backtrace_symbols_fd(frames, n, fds[i]);
#endif
    }
    if (log >= 0) {
        close(log);
        say(STDERR_FILENO, "Written to ");
        say(STDERR_FILENO, crash_log);
        say(STDERR_FILENO, "\n");
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void plat_catch_crashes(void) {
    snprintf(crash_log, sizeof(crash_log), "%s/mumoria-client.log", client_dir);
    if (access(client_dir, W_OK) != 0) {
        snprintf(crash_log, sizeof(crash_log), "mumoria-client.log");
    }
#ifdef HAVE_BACKTRACE
    void *warm[1];
    backtrace(warm, 1); // loads libgcc now, not inside the handler
#endif
    int sigs[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
    for (int i = 0; i < 5; i++) {
        signal(sigs[i], on_crash);
    }
}
