// Mumoria - a small terminal client.
//
// Any telnet client plays Mumoria; this one only saves setting up a terminal:
// it puts the terminal in raw mode, connects, and passes bytes both ways,
// dropping the server's telnet negotiation. Ctrl-] quits.
//
//     mumoria-client [host] [port]

#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

static struct termios saved;

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

    int iac = 0; // telnet: 1 after IAC, 2 inside an option, 3 in a subnegotiation, 4 IAC in it
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        FD_SET(fd, &fds);
        if (select(fd + 1, &fds, NULL, NULL, NULL) < 0) {
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
                        if (c == 255) {
                            out[m++] = c;
                        }
                        break;
                    case 2:
                        iac = 0;
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
