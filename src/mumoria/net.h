// Mumoria - connections: TCP, telnet, a key queue and a screen per player.
//
// Each connection has an I/O thread (net.c) that reads keys into the queue
// and sends what the screen has waiting, and a game thread (session.cpp) that
// takes keys from the queue. Both hold a reference; the last one out frees it.

#pragma once

#include "vterm.h"

#include <pthread.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MU_MAX_KEYS_PER_SEC 40
#define MU_KEY_QUEUE 512

enum { MU_KEY_CLOSED = -1, MU_KEY_TIMEOUT = -2, MU_KEY_POKED = -3 };

typedef struct MuConn {
    int fd;
    char ip[64];
    MuScreen screen;
    int wake_pipe[2];

    pthread_mutex_t lock;
    pthread_cond_t cond;
    unsigned char keys[MU_KEY_QUEUE];
    int key_head, key_tail;
    int closed;   // the socket is gone
    int kill;     // the server wants it closed
    int poked;    // something changed that the player should see
    int refs;

    time_t key_second;
    int keys_this_second;

    // telnet parser state
    int tn_state;
    int last_cr;
    int esc_state;
} MuConn;

int mu_net_listen(int port);
// Accepts forever, calling on_conn (in a new thread) for each connection.
void mu_net_serve(int listen_fd, void (*on_conn)(MuConn *conn));

// The next key; MU_KEY_CLOSED, MU_KEY_TIMEOUT after timeout_ms (<0: none),
// or MU_KEY_POKED when mu_conn_poke() was called and poke_wakes is set.
int mu_conn_key(MuConn *c, int timeout_ms, int poke_wakes);
int mu_conn_has_key(MuConn *c);
// Waits up to timeout_ms for a poke, leaving any keys queued: MU_KEY_POKED,
// MU_KEY_TIMEOUT or MU_KEY_CLOSED.
int mu_conn_wait(MuConn *c, int timeout_ms);
void mu_conn_poke(MuConn *c);
void mu_conn_kill(MuConn *c);
void mu_conn_release(MuConn *c);

#ifdef __cplusplus
}
#endif
