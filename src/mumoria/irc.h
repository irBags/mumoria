// Mumoria - the IRC bridge: world chat <-> an IRC channel, so members in the
// P3OL/AOL chat room of that name (the rooms are IRC) talk with the players.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*MuIrcHandler)(const char *nick, const char *text);

// Starts the bridge thread (reconnects by itself). Does nothing if host is empty.
void mu_irc_start(const char *host, int port, const char *channel, const char *nick, MuIrcHandler on_message);
// Sends "<who>: text" to the channel, if connected.
void mu_irc_say(const char *who, const char *text);

#ifdef __cplusplus
}
#endif
