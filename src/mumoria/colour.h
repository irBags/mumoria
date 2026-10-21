// Mumoria - how players are drawn (docs/MUMORIA_DESIGN.md section 3).
//
// Every "@" is bold, in its class's colour. Your own has no shield. Every
// other player stands on a shield (the square's background): light blue in
// a party, their guild's colour in a guild, faint white otherwise. Guilds
// pick from MU_GUILD_SHIELDS, which leaves out the party and plain colours.

#pragma once

#include <cstdint>
#include <string>

#define MU_PARTY_SHIELD 117 // xterm 256: light sky blue
#define MU_PLAIN_SHIELD 252 // faint white

struct MuShield {
    const char *name;
    int colour; // xterm 256 index
};

extern const MuShield MU_GUILD_SHIELDS[];
extern const int MU_GUILD_SHIELD_COUNT;

int mu_shield_by_name(const std::string &name); // 0 if not a guild colour
const char *mu_shield_name(int colour);
int mu_default_shield(int64_t guild);
std::string mu_shield_list();
