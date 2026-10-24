// Mumoria - a small terminal client.
//
// Any telnet client plays Mumoria; this one only saves setting up a terminal:
// it puts the terminal in raw mode, connects, and passes bytes both ways,
// dropping the server's telnet negotiation, except that it tells the server
// its window's size (NAWS), so a wide window gets the chat beside the map.
// Ctrl-] quits.
//
//     mumoria-client [host] [port]

#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

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
    const char *host = argc > 1 ? argv[1] : "localhost";
    const char *port = argc > 2 ? argv[2] : "4000";
    int fd = dial(host, port);
    if (fd < 0) {
        fprintf(stderr, "Cannot reach %s:%s\n", host, port);
        return 1;
    }

    tcgetattr(STDIN_FILENO, &saved);
    struct termios raw = saved;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    atexit(restore);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGWINCH, on_resize);

    int iac = 0; // telnet: 1 after IAC, 2 inside an option, 3 in a subnegotiation, 4 IAC in it
    int verb = 0;
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
                        break;
                    case 3:
                        iac = (c == 255) ? 4 : 3;
                        break;
                    default:
                        iac = (c == 240) ? 0 : 3;
                        break;
                }
            }
            (void) !write(STDOUT_FILENO, out, (size_t) m);
        }
    }
    close(fd);
    return 0;
}
