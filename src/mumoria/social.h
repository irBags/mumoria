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
    bool turn_based = false; // set by the leader: /party turns on|off
};

extern volatile time_t mu_shutdown_at;
extern int mu_guild_cost; // gold to found a guild (mumoria.conf guild_cost)
// shared gold added per level below the first, in hundredths (party_gold_per_level)
extern int mu_gold_level_bonus;

void mu_social_entered(MuSession *s);
void mu_social_left(MuSession *s, bool died);
void mu_social_command(MuSession *s, const std::string &line);
void mu_chat_from_irc(const char *nick, const char *text);
void mu_loot_rolls(int treasure_before, Coord_t const &center);

// Turn-based battles (a party's or guild's choice; the town never is).
bool mu_turn_based(MuSession *s);
// At the main prompt (lock held): the member who must act before s may act
// again, or null. Members not near an awake monster are not waited for.
MuSession *mu_round_waits_for(MuSession *s);
// The end of s's game turn: their action in this round is taken.
void mu_round_acted(MuSession *s);
// Waited too long for p: their action this round is skipped.
void mu_round_skip(MuSession *p);
// s (loaded) is in a battle round with another member and has not acted yet.
bool mu_round_my_turn(MuSession *s);

int mu_guild_perms(int64_t guild, int rank);
std::string mu_guild_name(int64_t guild);
// Creates a guild led by s, paying mu_guild_cost from what s carries (lock held, s loaded).
bool mu_guild_create(MuSession *s, const std::string &name, std::string &why);
