// Mumoria - chat, parties, guilds, trade requests, moderation.

#pragma once

#include "session.h"

#include <string>

// guild rank permissions
enum : int {
    GP_INVITE = 1,
    GP_KICK = 2,
    GP_PROMOTE = 4,
    GP_DEMOTE = 8,
    GP_DEPOSIT = 16,
    GP_WITHDRAW = 32,
    GP_MOTD = 64,
    GP_RANKS = 128,
    GP_ALL = 255,
};

struct MuParty {
    std::vector<MuSession *> members;
    MuSession *leader = nullptr;
};

extern volatile time_t mu_shutdown_at;
extern int mu_guild_cost; // gold to found a guild (mumoria.conf guild_cost)

void mu_social_entered(MuSession *s);
void mu_social_left(MuSession *s, bool died);
void mu_social_command(MuSession *s, const std::string &line);
void mu_chat_from_irc(const char *nick, const char *text);

int mu_guild_perms(int64_t guild, int rank);
std::string mu_guild_name(int64_t guild);
// Creates a guild led by s, paying mu_guild_cost from what s carries (lock held, s loaded).
bool mu_guild_create(MuSession *s, const std::string &name, std::string &why);
