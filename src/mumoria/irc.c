// Mumoria - the IRC bridge. See irc.h.

#include "irc.h"

#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

static char irc_host[128], irc_channel[64], irc_nick[32];
static int irc_port;
static MuIrcHandler irc_handler;
static int irc_fd = -1;
static int irc_joined = 0;
static pthread_mutex_t irc_lock = PTHREAD_MUTEX_INITIALIZER;

static void send_line(const char *line) {
    pthread_mutex_lock(&irc_lock);
    if (irc_fd >= 0) {
        size_t n = strlen(line);
        (void) !send(irc_fd, line, n, MSG_NOSIGNAL);
        (void) !send(irc_fd, "\r\n", 2, MSG_NOSIGNAL);
    }
    pthread_mutex_unlock(&irc_lock);
}

static int dial(void) {
    struct addrinfo hints, *res = NULL;
    char port[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof(port), "%d", irc_port);
    if (getaddrinfo(irc_host, port, &hints, &res) != 0) {
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

// ":nick!user@host PRIVMSG #chan :text"
static void handle(char *line) {
    char out[600];
    if (strncmp(line, "PING ", 5) == 0) {
        snprintf(out, sizeof(out), "PONG %s", line + 5);
        send_line(out);
        return;
    }
    if (line[0] != ':') {
        return;
    }
    char *space = strchr(line, ' ');
    if (space == NULL) {
        return;
    }
    *space = '\0';
    char *prefix = line + 1, *rest = space + 1;
    if (strncmp(rest, "001 ", 4) == 0) {
        snprintf(out, sizeof(out), "JOIN %s", irc_channel);
        send_line(out);
        irc_joined = 1;
        return;
    }
    if (strncmp(rest, "PRIVMSG ", 8) != 0) {
        return;
    }
    char *target = rest + 8;
    char *text = strstr(target, " :");
    if (text == NULL) {
        return;
    }
    *text = '\0';
    text += 2;
    if (strcasecmp(target, irc_channel) != 0) {
        return;
    }
    char *bang = strchr(prefix, '!');
    if (bang != NULL) {
        *bang = '\0';
    }
    if (strcasecmp(prefix, irc_nick) != 0 && irc_handler != NULL && text[0] != '\001') {
        irc_handler(prefix, text);
    }
}

static void *irc_thread(void *arg) {
    (void) arg;
    int backoff = 5;
    for (;;) {
        int fd = dial();
        if (fd < 0) {
            sleep((unsigned) backoff);
            backoff = backoff < 300 ? backoff * 2 : 300;
            continue;
        }
        backoff = 5;
        pthread_mutex_lock(&irc_lock);
        irc_fd = fd;
        pthread_mutex_unlock(&irc_lock);
        char line[600];
        snprintf(line, sizeof(line), "NICK %s", irc_nick);
        send_line(line);
        snprintf(line, sizeof(line), "USER %s 0 * :Mumoria game bridge", irc_nick);
        send_line(line);

        char buf[4096];
        int have = 0;
        for (;;) {
            ssize_t n = recv(fd, buf + have, sizeof(buf) - 1 - (size_t) have, 0);
            if (n <= 0) {
                break;
            }
            have += (int) n;
            buf[have] = '\0';
            char *start = buf, *eol;
            while ((eol = strstr(start, "\r\n")) != NULL) {
                *eol = '\0';
                handle(start);
                start = eol + 2;
            }
            have -= (int) (start - buf);
            memmove(buf, start, (size_t) have);
            if (have >= (int) sizeof(buf) - 1) {
                have = 0; // a line too long for IRC: dropped
            }
        }
        pthread_mutex_lock(&irc_lock);
        close(irc_fd);
        irc_fd = -1;
        irc_joined = 0;
        pthread_mutex_unlock(&irc_lock);
        sleep(5);
    }
    return NULL;
}

void mu_irc_start(const char *host, int port, const char *channel, const char *nick, MuIrcHandler on_message) {
    if (host == NULL || host[0] == '\0') {
        return;
    }
    snprintf(irc_host, sizeof(irc_host), "%s", host);
    snprintf(irc_channel, sizeof(irc_channel), "%s", channel);
    snprintf(irc_nick, sizeof(irc_nick), "%s", nick);
    irc_port = port;
    irc_handler = on_message;
    pthread_t t;
    pthread_create(&t, NULL, irc_thread, NULL);
    pthread_detach(t);
}

void mu_irc_say(const char *who, const char *text) {
    if (!irc_joined) {
        return;
    }
    char line[600];
    snprintf(line, sizeof(line), "PRIVMSG %s :%s: %s", irc_channel, who, text);
    for (char *p = line; *p != '\0'; p++) {
        if (*p == '\r' || *p == '\n') {
            *p = ' ';
        }
    }
    send_line(line);
}
