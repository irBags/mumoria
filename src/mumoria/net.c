// Mumoria - connections. See net.h.

#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define IAC 255
#define DONT 254
#define DO 253
#define WONT 252
#define WILL 251
#define SB 250
#define SE 240
#define OPT_ECHO 1
#define OPT_SGA 3

int mu_net_listen(int port) {
    signal(SIGPIPE, SIG_IGN);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t) port);
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0 || listen(fd, 64) < 0) {
        perror("bind/listen");
        close(fd);
        return -1;
    }
    return fd;
}

static void release_locked(MuConn *c) {
    if (--c->refs == 0) {
        pthread_mutex_unlock(&c->lock);
        close(c->fd);
        close(c->wake_pipe[0]);
        close(c->wake_pipe[1]);
        mu_vt_free(&c->screen);
        pthread_mutex_destroy(&c->lock);
        pthread_cond_destroy(&c->cond);
        free(c);
        return;
    }
    pthread_mutex_unlock(&c->lock);
}

void mu_conn_release(MuConn *c) {
    pthread_mutex_lock(&c->lock);
    release_locked(c);
}

static void push_key(MuConn *c, unsigned char k) {
    time_t now = time(NULL);
    if (now != c->key_second) {
        c->key_second = now;
        c->keys_this_second = 0;
    }
    if (++c->keys_this_second > MU_MAX_KEYS_PER_SEC) {
        return; // flooding: dropped
    }
    int next = (c->key_tail + 1) % MU_KEY_QUEUE;
    if (next == c->key_head) {
        return;
    }
    c->keys[c->key_tail] = k;
    c->key_tail = next;
}

// Arrow keys become Umoria's keypad digits.
static int arrow(unsigned char ch) {
    switch (ch) {
        case 'A': return '8';
        case 'B': return '2';
        case 'C': return '6';
        case 'D': return '4';
        default: return 0;
    }
}

// Telnet commands are dropped; CR LF and CR NUL arrive as one CR.
static void feed(MuConn *c, const unsigned char *buf, int n) {
    pthread_mutex_lock(&c->lock);
    for (int i = 0; i < n; i++) {
        unsigned char ch = buf[i];
        switch (c->tn_state) {
            case 0:
                if (ch == IAC) {
                    c->tn_state = 1;
                    continue;
                }
                break;
            case 1:
                if (ch == IAC) {
                    c->tn_state = 0;
                    break; // a literal 255; dropped below as not printable
                }
                c->tn_state = (ch == SB) ? 3 : (ch >= WILL && ch <= DONT) ? 2 : 0;
                continue;
            case 2:
                c->tn_state = 0;
                continue;
            case 3:
                if (ch == IAC) {
                    c->tn_state = 4;
                }
                continue;
            case 4:
                c->tn_state = (ch == SE) ? 0 : 3;
                continue;
        }

        if (c->last_cr && (ch == '\n' || ch == '\0')) {
            c->last_cr = 0;
            continue;
        }
        c->last_cr = (ch == '\r');
        if (ch == '\n') {
            ch = '\r';
        }

        if (c->esc_state == 1) {
            if (ch == '[' || ch == 'O') {
                c->esc_state = 2;
                continue;
            }
            push_key(c, 27);
            c->esc_state = 0;
        } else if (c->esc_state == 2) {
            c->esc_state = 0;
            int a = arrow(ch);
            if (a != 0) {
                push_key(c, (unsigned char) a);
            }
            continue;
        }
        if (ch == 27) {
            c->esc_state = 1;
            continue;
        }
        if (ch == 255) {
            continue;
        }
        push_key(c, ch);
    }
    // a lone ESC at the end of what arrived is a real ESC
    if (c->esc_state == 1) {
        push_key(c, 27);
        c->esc_state = 0;
    }
    pthread_cond_broadcast(&c->cond);
    pthread_mutex_unlock(&c->lock);
}

static void *io_thread(void *arg) {
    MuConn *c = arg;
    char out[8192];
    unsigned char in[1024];
    int pending = 0, sent = 0;

    for (;;) {
        pthread_mutex_lock(&c->lock);
        int kill = c->kill;
        pthread_mutex_unlock(&c->lock);

        if (pending == sent) {
            pending = mu_vt_take_output(&c->screen, out, sizeof(out));
            sent = 0;
        }
        if (c->screen.overflow) {
            fprintf(stderr, "mumoria: %s dropped: it fell %d bytes behind\n", c->ip, MU_OUT_LIMIT);
            break;
        }
        if (kill && pending == sent) {
            break;
        }

        struct pollfd fds[2];
        fds[0].fd = c->fd;
        fds[0].events = POLLIN | (pending > sent ? POLLOUT : 0);
        fds[1].fd = c->wake_pipe[0];
        fds[1].events = POLLIN;
        if (poll(fds, 2, kill ? 200 : 1000) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (fds[1].revents & POLLIN) {
            char drain[64];
            (void) !read(c->wake_pipe[0], drain, sizeof(drain));
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break;
        }
        if (fds[0].revents & POLLIN) {
            ssize_t n = recv(c->fd, in, sizeof(in), 0);
            if (n <= 0) {
                break;
            }
            feed(c, in, (int) n);
        }
        if ((fds[0].revents & POLLOUT) && pending > sent) {
            ssize_t n = send(c->fd, out + sent, (size_t) (pending - sent), MSG_DONTWAIT);
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }
            if (n > 0) {
                sent += (int) n;
            }
        }
    }

    shutdown(c->fd, SHUT_RDWR);
    pthread_mutex_lock(&c->lock);
    c->closed = 1;
    pthread_cond_broadcast(&c->cond);
    release_locked(c);
    return NULL;
}

typedef struct {
    MuConn *conn;
    void (*on_conn)(MuConn *);
} Start;

static void *game_thread(void *arg) {
    Start s = *(Start *) arg;
    free(arg);
    s.on_conn(s.conn);
    mu_conn_release(s.conn);
    return NULL;
}

void mu_net_serve(int listen_fd, void (*on_conn)(MuConn *conn)) {
    for (;;) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int fd = accept(listen_fd, (struct sockaddr *) &addr, &len);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        MuConn *c = calloc(1, sizeof(MuConn));
        c->fd = fd;
        inet_ntop(AF_INET, &addr.sin_addr, c->ip, sizeof(c->ip));
        if (pipe(c->wake_pipe) != 0) {
            close(fd);
            free(c);
            continue;
        }
        fcntl(c->wake_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(c->wake_pipe[1], F_SETFL, O_NONBLOCK);
        pthread_mutex_init(&c->lock, NULL);
        pthread_cond_init(&c->cond, NULL);
        mu_vt_init(&c->screen, c->wake_pipe[1]);
        c->refs = 2;

        static const char hello[] = {(char) IAC, (char) WILL, OPT_ECHO, (char) IAC, (char) WILL, OPT_SGA, (char) IAC, (char) DO, OPT_SGA};
        mu_vt_raw(&c->screen, hello, sizeof(hello));

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 8 * 1024 * 1024); // Umoria keeps big arrays on the stack
        pthread_t t;
        pthread_create(&t, &attr, io_thread, c);
        Start *s = malloc(sizeof(Start));
        s->conn = c;
        s->on_conn = on_conn;
        pthread_create(&t, &attr, game_thread, s);
        pthread_attr_destroy(&attr);
    }
}

int mu_conn_key(MuConn *c, int timeout_ms, int poke_wakes) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    if (timeout_ms >= 0) {
        until.tv_sec += timeout_ms / 1000;
        until.tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
    }
    pthread_mutex_lock(&c->lock);
    int result;
    for (;;) {
        if (c->key_head != c->key_tail) {
            result = c->keys[c->key_head];
            c->key_head = (c->key_head + 1) % MU_KEY_QUEUE;
            break;
        }
        if (c->closed || c->kill) {
            result = MU_KEY_CLOSED;
            break;
        }
        if (poke_wakes && c->poked) {
            c->poked = 0;
            result = MU_KEY_POKED;
            break;
        }
        if (timeout_ms >= 0) {
            if (pthread_cond_timedwait(&c->cond, &c->lock, &until) != 0) {
                result = MU_KEY_TIMEOUT;
                break;
            }
        } else {
            pthread_cond_wait(&c->cond, &c->lock);
        }
    }
    pthread_mutex_unlock(&c->lock);
    return result;
}

int mu_conn_has_key(MuConn *c) {
    pthread_mutex_lock(&c->lock);
    int has = c->key_head != c->key_tail || c->closed || c->kill;
    pthread_mutex_unlock(&c->lock);
    return has;
}

int mu_conn_wait(MuConn *c, int timeout_ms) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000;
    until.tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&c->lock);
    int result = MU_KEY_TIMEOUT;
    while (!c->closed && !c->kill && !c->poked) {
        if (pthread_cond_timedwait(&c->cond, &c->lock, &until) != 0) {
            break;
        }
    }
    if (c->closed || c->kill) {
        result = MU_KEY_CLOSED;
    } else if (c->poked) {
        c->poked = 0;
        result = MU_KEY_POKED;
    }
    pthread_mutex_unlock(&c->lock);
    return result;
}

void mu_conn_poke(MuConn *c) {
    pthread_mutex_lock(&c->lock);
    c->poked = 1;
    pthread_cond_broadcast(&c->cond);
    pthread_mutex_unlock(&c->lock);
}

void mu_conn_kill(MuConn *c) {
    pthread_mutex_lock(&c->lock);
    c->kill = 1;
    pthread_cond_broadcast(&c->cond);
    pthread_mutex_unlock(&c->lock);
    char b = 1;
    (void) !write(c->wake_pipe[1], &b, 1);
}

