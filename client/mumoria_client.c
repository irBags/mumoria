// Mumoria - a small terminal client.
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
// answers WONT. Each is sounds/<name>.wav, next to this program (or in
// $MUMORIA_SOUNDS), played by a command: $MUMORIA_PLAYER, else paplay (Linux,
// WSL) or afplay (macOS). In the command %f is the file, %v the volume 0-100,
// %p it for paplay (0-65536) and %a for afplay (0.00-1.00). A missing file is
// silence, so any sound can be left out or replaced. See client/sounds/README.md.

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#define TELOPT_SOUND 212

static struct termios saved;
static volatile sig_atomic_t resized = 0;
static int naws = 0; // the server asked for the window's size

static void on_resize(int sig) {
    (void) sig;
    resized = 1;
}

// IAC SB NAWS width height IAC SE, a 255 doubled
static void send_size(int fd) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) != 0) {
        return;
    }
    unsigned char msg[24];
    int n = 0;
    msg[n++] = 255;
    msg[n++] = 250;
    msg[n++] = 31;
    unsigned int v[4] = {w.ws_col >> 8, w.ws_col & 0xff, w.ws_row >> 8, w.ws_row & 0xff};
    for (int i = 0; i < 4; i++) {
        msg[n++] = (unsigned char) v[i];
        if (v[i] == 255) {
            msg[n++] = 255;
        }
    }
    msg[n++] = 255;
    msg[n++] = 240;
    (void) send(fd, msg, (size_t) n, 0);
}

// --- sounds -------------------------------------------------------------------

static char sound_dir[1024];
static const char *player_cmd;

static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// sounds/ beside this program (Linux: /proc/self/exe), else ./sounds
static void find_sounds(const char *argv0) {
    const char *env = getenv("MUMORIA_SOUNDS");
    if (env != NULL && *env != '\0') {
        snprintf(sound_dir, sizeof(sound_dir), "%s", env);
        return;
    }
    char exe[900];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
    } else {
        snprintf(exe, sizeof(exe), "%s", argv0);
    }
    char *slash = strrchr(exe, '/');
    if (slash != NULL) {
        *slash = '\0';
        snprintf(sound_dir, sizeof(sound_dir), "%s/sounds", exe);
    } else {
        snprintf(sound_dir, sizeof(sound_dir), "sounds");
    }
}

static void pick_player(void) {
    player_cmd = getenv("MUMORIA_PLAYER");
    if (player_cmd != NULL && *player_cmd != '\0') {
        return;
    }
#ifdef __APPLE__
    player_cmd = "afplay -v %a %f";
#else
    player_cmd = "paplay --volume=%p %f";
#endif
}

// Plays sounds/<name>.wav in the background. The name comes from the server,
// so only letters, digits and _ are taken (it goes into a shell command).
static void play_sound(const char *name, int volume) {
    static char last[32];
    static long long last_at;
    if (name[0] == '\0' || strlen(name) >= sizeof(last) || strchr(sound_dir, '\'') != NULL) {
        return;
    }
    for (const char *p = name; *p != '\0'; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) {
            return;
        }
    }
    long long t = now_ms();
    if (strcmp(name, last) == 0 && t - last_at < 60) {
        return; // the same sound twice at once is one sound
    }
    snprintf(last, sizeof(last), "%s", name);
    last_at = t;

    char file[1200];
    snprintf(file, sizeof(file), "%s/%s.wav", sound_dir, name);
    struct stat st;
    if (stat(file, &st) != 0) {
        return;
    }
    volume = volume < 1 ? 1 : volume > 100 ? 100 : volume;
    char cmd[2048];
    int m = 0;
    for (const char *p = player_cmd; *p != '\0' && m < (int) sizeof(cmd) - 1300; p++) {
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
}

// IAC SB 212 "name volume" IAC SE, without the IACs
static void sound_message(const unsigned char *sb, int len) {
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
    play_sound(text, volume);
}

static void restore(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    printf("\033[0m\n");
}

static int dial(const char *host, const char *port) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) {
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *a = res; a != NULL; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
            break;
        }
        if (fd >= 0) {
            close(fd);
        }
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int main(int argc, char *argv[]) {
    const char *sound_env = getenv("MUMORIA_SOUND");
    int sound = sound_env == NULL || strcmp(sound_env, "off") != 0;
    int arg = 1;
    if (arg < argc && strcmp(argv[arg], "-q") == 0) {
        sound = 0;
        arg++;
    }
    const char *host = argc > arg ? argv[arg] : "localhost";
    const char *port = argc > arg + 1 ? argv[arg + 1] : "4000";
    find_sounds(argv[0]);
    pick_player();
    int fd = dial(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot reach %s:%s\n", host, port);
        return 1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC); // not held open by a sound player

    tcgetattr(STDIN_FILENO, &saved);
    struct termios raw = saved;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    atexit(restore);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN); // the sound players reap themselves
    signal(SIGWINCH, on_resize);

    int iac = 0; // telnet: 1 after IAC, 2 inside an option, 3 in a subnegotiation, 4 IAC in it
    int verb = 0;
    unsigned char sb[64]; // the subnegotiation being read
    int sb_len = 0;
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        FD_SET(fd, &fds);
        if (select(fd + 1, &fds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) {
                if (resized && naws) {
                    send_size(fd);
                }
                resized = 0;
                continue;
            }
            break;
        }
        unsigned char buf[4096];
        if (FD_ISSET(STDIN_FILENO, &fds)) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0 || memchr(buf, 29, (size_t) n) != NULL) { // Ctrl-]
                break;
            }
            if (send(fd, buf, (size_t) n, 0) <= 0) {
                break;
            }
        }
        if (FD_ISSET(fd, &fds)) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            unsigned char out[4096];
            int m = 0;
            for (ssize_t i = 0; i < n; i++) {
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
                        if (verb == 253 && c == 31 && !naws) { // DO NAWS: WILL, and the size
                            unsigned char will[3] = {255, 251, 31};
                            (void) send(fd, will, 3, 0);
                            naws = 1;
                            send_size(fd);
                        }
                        if (verb == 253 && c == TELOPT_SOUND) { // DO SOUND: WILL, unless -q
                            unsigned char reply[3] = {255, (unsigned char) (sound ? 251 : 252), TELOPT_SOUND};
                            (void) send(fd, reply, 3, 0);
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
                        if (c == 240 && sb_len > 0 && sb[0] == TELOPT_SOUND && sound) {
                            sound_message(sb, sb_len);
                        }
                        break;
                }
            }
            (void) !write(STDOUT_FILENO, out, (size_t) m);
        }
    }
    close(fd);
    return 0;
}
