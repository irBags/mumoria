// Mumoria - how players are drawn. See colour.h.

#include "colour.h"

#include "loot.h"
#include "session.h"

// Pastels: every class colour (dark) reads on them, and none is close to
// the party's light blue or the plain faint white.
const MuShield MU_GUILD_SHIELDS[] = {
    {"rose", 217},  {"salmon", 210}, {"coral", 209},  {"orange", 214}, {"peach", 216},    {"gold", 220},
    {"lemon", 229}, {"khaki", 186},  {"lime", 191},   {"mint", 158},   {"sage", 151},     {"lavender", 183},
    {"violet", 177}, {"orchid", 213}, {"pink", 218},  {"tan", 180},
};
const int MU_GUILD_SHIELD_COUNT = (int) (sizeof(MU_GUILD_SHIELDS) / sizeof(MU_GUILD_SHIELDS[0]));

// Umoria's classes: Warrior, Mage, Priest, Rogue, Ranger, Paladin - the
// python port's hues (python-umoria/program.py CLASS_COLOR). Bright for your
// own "@" on the terminal's background, dark for the others' on a shield.
static const int CLASS_BRIGHT[6] = {196, 33, 214, 135, 46, 44};
static const int CLASS_DARK[6] = {124, 19, 94, 54, 22, 23};

int mu_shield_by_name(const std::string &name) {
    for (int i = 0; i < MU_GUILD_SHIELD_COUNT; i++) {
        if (name == MU_GUILD_SHIELDS[i].name) {
            return MU_GUILD_SHIELDS[i].colour;
        }
    }
    return 0;
}

const char *mu_shield_name(int colour) {
    for (int i = 0; i < MU_GUILD_SHIELD_COUNT; i++) {
        if (MU_GUILD_SHIELDS[i].colour == colour) {
            return MU_GUILD_SHIELDS[i].name;
        }
    }
    return "none";
}

int mu_default_shield(int64_t guild) {
    return MU_GUILD_SHIELDS[guild % MU_GUILD_SHIELD_COUNT].colour;
}

std::string mu_shield_list() {
    std::string out;
    for (int i = 0; i < MU_GUILD_SHIELD_COUNT; i++) {
        out += (i ? " " : "") + std::string(MU_GUILD_SHIELDS[i].name);
    }
    return out;
}

static int class_of(int id) {
    return id >= 0 && id < 6 ? id : 0;
}

uint32_t muTileAttr(Coord_t const &coord, char ch) {
    if (ch != '@') {
        return mu_loot_tile_attr(coord, ch); // loot.cpp
    }
    if (coord.y == py.pos.y && coord.x == py.pos.x && dg.floor[coord.y][coord.x].creature_id == 1) {
        return MU_ATTR_BOLD | MU_ATTR_FG(CLASS_BRIGHT[class_of(py.misc.class_id)]);
    }
    MuSession *p = mu_player_at(coord);
    if (p == nullptr) {
        return 0;
    }
    int shield = p->party != nullptr ? MU_PARTY_SHIELD : p->guild_shield != 0 ? p->guild_shield : MU_PLAIN_SHIELD;
    return MU_ATTR_BOLD | MU_ATTR_FG(CLASS_DARK[class_of(p->ctx.py.misc.class_id)]) | MU_ATTR_BG(shield);
}
