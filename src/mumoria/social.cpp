// Mumoria - chat, parties, guilds, trade requests, moderation.
//
// Everything here runs on a player's thread with the world lock held (it is
// reached from muCommand, inside the engine). Chat lines go to the chat pane
// under Umoria's screen (vterm.c), which any thread may write.

#include "social.h"

#include "boss.h"
#include "colour.h"
#include "irc.h"
#include "mu_config.h"
#include "screens.h"

#include <sstream>

volatile time_t mu_shutdown_at = 0;
int mu_guild_cost = 1000;

static std::vector<std::string> words(const std::string &line, size_t max_split) {
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string w;
    while (out.size() + 1 < max_split && in >> w) {
        out.push_back(w);
    }
    std::string rest;
    std::getline(in, rest);
    size_t start = rest.find_first_not_of(' ');
    if (start != std::string::npos) {
        out.push_back(rest.substr(start));
    }
    return out;
}

static std::string lower(std::string s) {
    for (char &c : s) {
        c = (char) tolower((unsigned char) c);
    }
    return s;
}

static MuSession *online(const std::string &name) {
    pthread_mutex_lock(&mu_sessions_lock);
    MuSession *p = mu_find_online(name.c_str());
    pthread_mutex_unlock(&mu_sessions_lock);
    return p;
}

static MuSession *by_account(int64_t account) {
    MuSession *found = nullptr;
    pthread_mutex_lock(&mu_sessions_lock);
    for (MuSession *p : mu_sessions) {
        if (p->account.id == account) {
            found = p;
        }
    }
    pthread_mutex_unlock(&mu_sessions_lock);
    return found;
}

static void say(MuSession *s, const std::string &line) {
    mu_system(s, line);
}

// --- chat -------------------------------------------------------------------

static bool may_chat(MuSession *s) {
    time_t now = time(nullptr);
    if (s->account.muted_until > now) {
        say(s, "You are muted for another " + std::to_string((s->account.muted_until - now + 59) / 60) + " minutes.");
        return false;
    }
    if (now - s->chat_window >= 5) {
        s->chat_window = now;
        s->chat_count = 0;
    }
    if (++s->chat_count > MU_CHAT_BURST) {
        say(s, "Slow down a little.");
        return false;
    }
    return true;
}

static void to_level(MuSession *s, const std::string &text) {
    std::string line = s->char_name + " says: " + text;
    for (MuSession *p : s->level->players) {
        say(p, line);
    }
}

static void to_world(MuSession *s, const std::string &text) {
    std::string line = "[World] " + s->char_name + ": " + text;
    mu_system_all(line);
    mu_irc_say(s->char_name.c_str(), text.c_str());
}

void mu_chat_from_irc(const char *nick, const char *text) {
    mu_system_all(std::string("[AOL] ") + nick + ": " + text);
}

static void to_party(MuSession *s, const std::string &text) {
    if (s->party == nullptr) {
        say(s, "You are not in a party.");
        return;
    }
    for (MuSession *p : s->party->members) {
        say(p, "[Party] " + s->char_name + ": " + text);
    }
}

static void to_guild_members(int64_t guild, const std::string &line) {
    pthread_mutex_lock(&mu_sessions_lock);
    for (MuSession *p : mu_sessions) {
        if (p->guild_id == guild) {
            mu_vt_chat(&p->conn->screen, line.c_str());
        }
    }
    pthread_mutex_unlock(&mu_sessions_lock);
}

static void to_guild(MuSession *s, const std::string &text) {
    if (s->guild_id == 0) {
        say(s, "You are not in a guild.");
        return;
    }
    to_guild_members(s->guild_id, "[Guild] " + s->char_name + ": " + text);
}

static void tell(MuSession *s, const std::string &who, const std::string &text) {
    MuSession *t = online(who);
    if (t == nullptr || !t->in_game) {
        say(s, who + " is not in the game.");
        return;
    }
    say(t, "[From " + s->char_name + "] " + text);
    say(s, "[To " + t->char_name + "] " + text);
    t->reply_to = s->char_name;
}

// Shields follow membership: after a party or guild change every map is
// redrawn (they are rare, and a redraw sends only the changed squares).
static void restyle_everyone() {
    pthread_mutex_lock(&mu_sessions_lock);
    for (MuSession *p : mu_sessions) {
        if (p->in_game) {
            p->view_stale = true;
            mu_conn_poke(p->conn);
        }
    }
    pthread_mutex_unlock(&mu_sessions_lock);
}

// --- parties ----------------------------------------------------------------

static void party_drop(MuSession *s) {
    MuParty *party = s->party;
    if (party == nullptr) {
        return;
    }
    party->members.erase(std::remove(party->members.begin(), party->members.end(), s), party->members.end());
    s->party = nullptr;
    for (MuSession *p : party->members) {
        say(p, s->char_name + " has left the party.");
    }
    if (party->members.size() <= 1) {
        for (MuSession *p : party->members) {
            say(p, "The party is disbanded.");
            p->party = nullptr;
        }
        delete party;
    } else if (party->leader == s) {
        party->leader = party->members[0];
        say(party->leader, "You now lead the party.");
    }
}

static void party_invite(MuSession *s, const std::string &who) {
    MuSession *t = online(who);
    if (t == nullptr || !t->in_game || t == s) {
        say(s, "Nobody by that name is in the game.");
        return;
    }
    if (t->party != nullptr) {
        say(s, t->char_name + " is already in a party.");
        return;
    }
    if (s->party != nullptr && s->party->leader != s) {
        say(s, "Only the party's leader invites.");
        return;
    }
    if (s->party != nullptr && (int) s->party->members.size() >= MU_PARTY_MAX) {
        say(s, "The party is full.");
        return;
    }
    t->invited_by = s->account.id;
    say(t, s->char_name + " invites you to a party. Type /accept or /decline.");
    say(s, "You invite " + t->char_name + ".");
}

static void party_accept(MuSession *s) {
    MuSession *inviter = s->invited_by != 0 ? by_account(s->invited_by) : nullptr;
    s->invited_by = 0;
    if (inviter == nullptr || !inviter->in_game) {
        say(s, "That invitation is gone.");
        return;
    }
    if (inviter->party == nullptr) {
        inviter->party = new MuParty();
        inviter->party->leader = inviter;
        inviter->party->members.push_back(inviter);
    }
    if ((int) inviter->party->members.size() >= MU_PARTY_MAX) {
        say(s, "The party is full.");
        return;
    }
    party_drop(s);
    s->party = inviter->party;
    for (MuSession *p : s->party->members) {
        say(p, s->char_name + " has joined the party.");
    }
    s->party->members.push_back(s);
    say(s, "You join " + inviter->char_name + "'s party.");
}

static void party_kick(MuSession *s, const std::string &who) {
    if (s->party == nullptr || s->party->leader != s) {
        say(s, "Only the party's leader can do that.");
        return;
    }
    MuSession *t = online(who);
    if (t == nullptr || t->party != s->party || t == s) {
        say(s, "They are not in your party.");
        return;
    }
    say(t, "You were removed from the party.");
    party_drop(t);
}

static void party_list(MuSession *s) {
    if (s->party == nullptr) {
        say(s, "You are not in a party.");
        return;
    }
    std::string line = "Party:";
    for (MuSession *p : s->party->members) {
        line += " " + p->char_name + (p == s->party->leader ? "*" : "");
    }
    say(s, line);
}

// Umoria's playerGainKillExperience(), on a base already shared out.
static void gain(int64_t exp) {
    int32_t quotient = (int32_t) (exp / py.misc.level);
    int32_t remainder = (int32_t) (exp % py.misc.level);
    remainder = remainder * 0x10000L / py.misc.level + py.misc.exp_fraction;
    if (remainder >= 0x10000L) {
        quotient++;
        py.misc.exp_fraction = (uint16_t) (remainder - 0x10000L);
    } else {
        py.misc.exp_fraction = (uint16_t) remainder;
    }
    py.misc.exp += quotient;
}

// The python port's rule: members on the level share each kill evenly, with
// 6% more for each member (python-umoria/mp_monsters.py exp_share).
void muKillExperience(Creature_t const &creature) {
    MuSession *s = mu_self;
    std::vector<MuSession *> here;
    if (s->party != nullptr) {
        for (MuSession *p : s->party->members) {
            if (p->level == s->level) {
                here.push_back(p);
            }
        }
    }
    int64_t n = here.size() > 1 ? (int64_t) here.size() : 1;
    int64_t base = (int64_t) creature.kill_exp_value * creature.level * (100 + (n > 1 ? 6 * n : 0)) / (100 * n);
    gain(base);
    if (n == 1) {
        return;
    }
    for (MuSession *p : here) {
        if (p != s) {
            p->pending_exp += base / (p->ctx.py.misc.level > 0 ? p->ctx.py.misc.level : 1);
            mu_conn_poke(p->conn);
        }
    }
}

// --- sharing: a party, or guild members, on the same level ----------------------

int mu_gold_level_bonus = 50;

static std::vector<MuSession *> sharers(MuSession *s) {
    std::vector<MuSession *> out{s};
    if (s->level == nullptr) {
        return out;
    }
    for (MuSession *p : s->level->players) {
        bool party = s->party != nullptr && p->party == s->party;
        bool guild = s->guild_id != 0 && p->guild_id == s->guild_id;
        if (p != s && (party || guild)) {
            out.push_back(p);
        }
    }
    return out;
}

// All gold is shared: the find plus mu_gold_level_bonus for each level below
// the first, split evenly. Pieces below one are kept per player in hundredths.
std::string muGoldFound(int32_t gold, const char *description) {
    MuSession *s = mu_self;
    std::vector<MuSession *> group = sharers(s);
    if (group.size() == 1) {
        py.misc.au += gold;
        return "You have found " + std::to_string(gold) + " gold pieces worth of " + description;
    }
    int depth = s->level != nullptr ? s->level->depth : 0;
    int64_t pool = (int64_t) gold * 100 + (int64_t) (depth > 1 ? depth - 1 : 0) * mu_gold_level_bonus;
    int64_t each = pool / (int64_t) group.size();
    int32_t mine = 0;
    for (MuSession *p : group) {
        p->gold_fraction += each;
        auto whole = (int32_t) (p->gold_fraction / 100);
        p->gold_fraction %= 100;
        if (p == s) {
            mine = whole;
            py.misc.au += whole;
        } else {
            p->ctx.py.misc.au += whole; // not loaded: the copy is theirs
            p->gold_dirty = true;
            say(p, "[Loot] " + s->char_name + " found " + std::to_string(gold) + " gold; your share is " + std::to_string(whole) + ".");
            mu_conn_poke(p->conn);
        }
    }
    return "You found " + std::to_string(gold) + " gold (" + description + "); your share is " + std::to_string(mine) + ".";
}

// Items a monster dropped (placed since treasure_before, near where it died)
// are rolled for: d100 each, ties roll again, the highest takes it.
void mu_loot_rolls(int treasure_before, Coord_t const &center) {
    MuSession *s = mu_self;
    std::vector<MuSession *> group = sharers(s);
    if (group.size() < 2 || game.treasure.current_id <= treasure_before) {
        return;
    }
    for (int y = center.y - 4; y <= center.y + 4; y++) {
        for (int x = center.x - 4; x <= center.x + 4; x++) {
            if (y <= 0 || x <= 0 || y >= dg.height - 1 || x >= dg.width - 1) {
                continue;
            }
            int id = dg.floor[y][x].treasure_id;
            if (id < treasure_before || id >= game.treasure.current_id) {
                continue;
            }
            Inventory_t item = game.treasure.list[id];
            if (item.category_id >= TV_MAX_PICK_UP) {
                continue; // gold is shared as it is picked up
            }

            std::vector<MuSession *> rolling = group;
            std::string rolls;
            MuSession *winner = nullptr;
            while (winner == nullptr) {
                int best = -1;
                std::vector<MuSession *> top;
                for (MuSession *p : rolling) {
                    int roll = randomNumber(100);
                    rolls += p->char_name + " " + std::to_string(roll) + ", ";
                    if (roll > best) {
                        best = roll;
                        top = {p};
                    } else if (roll == best) {
                        top.push_back(p);
                    }
                }
                if (top.size() == 1) {
                    winner = top[0];
                } else {
                    rolling = top;
                    rolls += "tie: ";
                }
            }

            bool taken = false;
            auto carry = [&] {
                if (inventoryCanCarryItemCount(item)) {
                    Inventory_t copy = item;
                    inventoryCarryItem(copy);
                    taken = true;
                }
            };
            mu_with_player(s, winner, carry);
            if (taken) {
                (void) dungeonDeleteObject(Coord_t{y, x});
            }
            obj_desc_t desc;
            itemDescription(desc, item, true);
            std::string what = desc;
            if (!what.empty() && what.back() == '.') {
                what.pop_back();
            }
            std::string line = "[Loot] " + what + ": " + rolls + winner->char_name +
                               (taken ? " wins it." : " wins, but has no room: it stays on the floor.");
            for (MuSession *p : group) {
                say(p, line);
            }
        }
    }
}

// --- turn-based battles -----------------------------------------------------------
//
// Moving is never turn-based: nobody waits for anyone to walk. A party (or
// guild-mates on a level, outside a party) whose leader turned battle turns
// on fights in rounds: each member near an awake monster takes one action a
// round; members farther away are left out until they come near.

bool mu_turn_based(MuSession *s) {
    if (s->level == nullptr || s->level->depth == 0) {
        return false;
    }
    if (s->party != nullptr) {
        return s->party->turn_based;
    }
    return s->guild_id != 0 && s->guild_turns;
}

// Those s fights in rounds with: on the level, in the same party or guild.
static std::vector<MuSession *> round_group(MuSession *s) {
    std::vector<MuSession *> out;
    if (!mu_turn_based(s)) {
        return out;
    }
    for (MuSession *p : s->level->players) {
        bool same = s->party != nullptr ? p->party == s->party : p->party == nullptr && p->guild_id == s->guild_id;
        if (same) {
            out.push_back(p);
        }
    }
    return out;
}

static bool in_battle(MuSession *p) {
    Coord_t at = p == mu_self ? py.pos : p->ctx.py.pos;
    for (int i = config::monsters::MON_MIN_INDEX_ID; i < next_free_monster_id; i++) {
        Monster_t const &m = monsters[i];
        if (m.hp >= 0 && m.sleep_count == 0 && coordDistanceBetween(at, m.pos) <= MU_BATTLE_RANGE) {
            return true;
        }
    }
    return false;
}

MuSession *mu_round_waits_for(MuSession *s) {
    std::vector<MuSession *> group = round_group(s);
    if (group.size() < 2 || !s->acted || !in_battle(s)) {
        return nullptr;
    }
    for (MuSession *p : group) {
        if (p != s && !p->acted && !p->in_building && in_battle(p)) {
            return p;
        }
    }
    // everyone in the battle has acted: a new round
    for (MuSession *p : group) {
        p->acted = false;
    }
    return nullptr;
}

void mu_round_acted(MuSession *s) {
    std::vector<MuSession *> group = round_group(s);
    if (group.size() < 2 || !in_battle(s)) {
        s->acted = false;
        return;
    }
    s->acted = true;
    for (MuSession *p : group) {
        if (p != s) {
            mu_conn_poke(p->conn); // the next one in the round may go
        }
    }
}

void mu_round_skip(MuSession *p) {
    p->acted = true;
    say(p, "Your turn in the battle was skipped.");
}

static void turns_command(MuSession *s, bool guild, const std::string &arg) {
    if (arg != "on" && arg != "off") {
        say(s, std::string("Battle turns are ") + (mu_turn_based(s) ? "on" : "off") + " for you. Use /" + (guild ? "guild" : "party") +
                   " turns on|off.");
        return;
    }
    bool on = arg == "on";
    if (guild) {
        if ((mu_guild_perms(s->guild_id, s->guild_rank) & GP_RANKS) == 0 || s->guild_id == 0) {
            say(s, "Only the guild's leaders set battle turns.");
            return;
        }
        mu_db_run("UPDATE guild SET turn_based = ? WHERE id = ?", "ii", (int64_t) on, s->guild_id);
        pthread_mutex_lock(&mu_sessions_lock);
        for (MuSession *p : mu_sessions) {
            if (p->guild_id == s->guild_id) {
                p->guild_turns = on;
                p->acted = false;
                mu_vt_chat(&p->conn->screen, ("[Guild] Battle turns are now " + arg + ".").c_str());
            }
        }
        pthread_mutex_unlock(&mu_sessions_lock);
        return;
    }
    if (s->party == nullptr || s->party->leader != s) {
        say(s, "Only the party's leader sets battle turns.");
        return;
    }
    s->party->turn_based = on;
    for (MuSession *p : s->party->members) {
        p->acted = false;
        say(p, "[Party] Battle turns are now " + arg + ".");
    }
}

// --- player against player ------------------------------------------------------

// Chosen at creation; changed at most once a day.
static void pvp_command(MuSession *s, const std::string &arg) {
    time_t now = time(nullptr);
    int64_t wait = s->pvp_changed_at + 86400 - (int64_t) now;
    if (arg != "on" && arg != "off") {
        say(s, std::string("PvP is ") + (s->pvp ? "on" : "off") +
                   (wait > 0 ? "; you can change it in " + std::to_string((wait + 3599) / 3600) + " hours." : ". /pvp on or /pvp off."));
        return;
    }
    bool want = arg == "on";
    if (want == s->pvp) {
        say(s, std::string("PvP is already ") + arg + ".");
        return;
    }
    if (wait > 0) {
        say(s, "PvP can be changed once a day; next in " + std::to_string((wait + 3599) / 3600) + " hours.");
        return;
    }
    s->pvp = want;
    s->pvp_changed_at = (int64_t) now;
    mu_db_run("UPDATE character SET pvp = ?, pvp_changed_at = ? WHERE id = ?", "iii", (int64_t) want, (int64_t) now, s->char_id);
    say(s, std::string("PvP is now ") + arg + ".");
}

// A player is attacked only on purpose: this key, and a direction. Walking
// into one never attacks.
static void pvp_attack(MuSession *s) {
    game.player_free_turn = true;
    int dir;
    if (!getAllDirections("Attack which way?", dir)) {
        return;
    }
    Coord_t at{py.pos.y + (dir <= 3 ? 1 : dir >= 7 ? -1 : 0), py.pos.x + (dir % 3 == 1 ? -1 : dir % 3 == 0 ? 1 : 0)};
    MuSession *t = mu_player_at(at);
    if (t == nullptr) {
        printMessage("There is nobody there.");
    } else if (s->level->depth == 0) {
        printMessage("The town watch will not have fighting here.");
    } else if (!s->pvp) {
        printMessage("You do not fight other players (/pvp on).");
    } else if (!t->pvp) {
        printMessage((t->char_name + " does not fight other players.").c_str());
    } else if (s->party != nullptr && t->party == s->party) {
        printMessage((t->char_name + " is in your party.").c_str());
    } else if (py.flags.afraid > 0) {
        printMessage("You are too afraid!");
    } else {
        game.player_free_turn = false;
        int ac = t->ctx.py.misc.ac + t->ctx.py.misc.magical_ac;
        int damage = muPlayerMeleeDamage(ac, t->char_name.c_str());
        if (damage > 0) {
            t->pending_pvp_damage += damage;
            t->pvp_attacker = s->char_name;
            t->event_pending = true;
        }
        say(t, damage > 0 ? s->char_name + " strikes you for " + std::to_string(damage) + "!" : s->char_name + " attacks you, and misses.");
        mu_conn_poke(t->conn);
        mu_db_audit(s->char_name.c_str(), "pvp", t->char_name.c_str(), std::to_string(damage).c_str());
    }
}

// --- guilds -----------------------------------------------------------------

int mu_guild_perms(int64_t guild, int rank) {
    return (int) mu_db_int("SELECT perms FROM guild_rank WHERE guild = ? AND rank = ?", 0, "ii", guild, (int64_t) rank);
}

std::string mu_guild_name(int64_t guild) {
    std::string name;
    MuRows *r = mu_db_query("SELECT name FROM guild WHERE id = ?", "i", guild);
    if (mu_db_next(r)) {
        name = mu_db_col_text(r, 0);
    }
    mu_db_done(r);
    return name;
}

static void load_guild(MuSession *s) {
    s->guild_id = 0;
    s->guild_rank = 0;
    s->guild_shield = 0;
    s->guild_turns = false;
    MuRows *r = mu_db_query("SELECT m.guild, m.rank, g.shield, g.turn_based FROM guild_member m JOIN guild g ON g.id = m.guild WHERE m.account = ?",
                            "i", s->account.id);
    if (mu_db_next(r)) {
        s->guild_id = mu_db_col_int(r, 0);
        s->guild_rank = (int) mu_db_col_int(r, 1);
        s->guild_turns = mu_db_col_int(r, 3) != 0;
        int shield = (int) mu_db_col_int(r, 2);
        s->guild_shield = shield != 0 ? shield : mu_default_shield(s->guild_id);
    }
    mu_db_done(r);
}

static void refresh_guild(int64_t account) {
    MuSession *p = by_account(account);
    if (p != nullptr) {
        load_guild(p);
    }
}

bool mu_guild_create(MuSession *s, const std::string &name, std::string &why) {
    char folded[33];
    if (s->guild_id != 0) {
        why = "You are already in a guild.";
        return false;
    }
    if (name.size() < 3 || name.size() > 24) {
        why = "A guild name is 3 to 24 characters.";
        return false;
    }
    mu_fold(name.c_str(), folded, sizeof(folded));
    if (mu_db_int("SELECT COUNT(*) FROM guild WHERE folded = ?", 1, "s", folded) != 0) {
        why = "That guild name is taken.";
        return false;
    }
    if (py.misc.au < mu_guild_cost) {
        why = "Founding a guild costs " + std::to_string(mu_guild_cost) + " gold.";
        return false;
    }
    static const struct {
        const char *title;
        int perms;
    } ranks[5] = {{"Leader", GP_ALL},
                  {"Officer", GP_INVITE | GP_KICK | GP_PROMOTE | GP_DEMOTE | GP_DEPOSIT | GP_WITHDRAW | GP_MOTD},
                  {"Veteran", GP_INVITE | GP_DEPOSIT | GP_WITHDRAW},
                  {"Member", GP_DEPOSIT},
                  {"Recruit", 0}};

    if (mu_db_begin() != 0) {
        why = "The guild registry is busy.";
        return false;
    }
    int64_t now = (int64_t) time(nullptr);
    py.misc.au -= mu_guild_cost;
    bool ok = mu_db_run("INSERT INTO guild (name, folded, leader, created) VALUES (?, ?, ?, ?)", "ssii", name.c_str(), folded, s->account.id, now) == 1;
    int64_t guild = mu_db_last_id();
    for (int i = 0; ok && i < 5; i++) {
        ok = mu_db_run("INSERT INTO guild_rank (guild, rank, title, perms) VALUES (?, ?, ?, ?)", "iisi", guild, (int64_t) i, ranks[i].title,
                       (int64_t) ranks[i].perms) == 1;
    }
    ok = ok && mu_db_run("INSERT INTO guild_member (account, guild, rank, joined) VALUES (?, ?, 0, ?)", "iii", s->account.id, guild, now) == 1;
    ok = ok && mu_char_store(s);
    if (ok) {
        ok = mu_db_commit() == 0; // a failed commit rolls back
    } else {
        mu_db_rollback();
    }
    if (!ok) {
        py.misc.au += mu_guild_cost;
        why = "The guild could not be founded.";
        return false;
    }
    mu_db_ledger(s->account.id, s->char_name.c_str(), "guild-found", -mu_guild_cost, "", name.c_str());
    s->guild_id = guild;
    s->guild_rank = 0;
    return true;
}

static std::string rank_title(int64_t guild, int rank) {
    std::string t;
    MuRows *r = mu_db_query("SELECT title FROM guild_rank WHERE guild = ? AND rank = ?", "ii", guild, (int64_t) rank);
    if (mu_db_next(r)) {
        t = mu_db_col_text(r, 0);
    }
    mu_db_done(r);
    return t;
}

static bool member_rank(int64_t guild, int64_t account, int &rank) {
    rank = (int) mu_db_int("SELECT rank FROM guild_member WHERE guild = ? AND account = ?", -1, "ii", guild, account);
    return rank >= 0;
}

static bool need(MuSession *s, int perm) {
    if (s->guild_id == 0) {
        say(s, "You are not in a guild.");
        return false;
    }
    if ((mu_guild_perms(s->guild_id, s->guild_rank) & perm) == 0) {
        say(s, "Your rank may not do that.");
        return false;
    }
    return true;
}

static void guild_command(MuSession *s, const std::vector<std::string> &w) {
    std::string sub = w.size() > 1 ? lower(w[1]) : "info";
    std::string arg = w.size() > 2 ? w[2] : "";
    MuAccount target{};

    if (sub == "create") {
        std::string why;
        if (mu_guild_create(s, arg, why)) {
            say(s, "You found the guild " + arg + ".");
            mu_db_audit(s->account.name, "guild-create", arg.c_str(), "");
        } else {
            say(s, why);
        }
    } else if (sub == "invite") {
        if (!need(s, GP_INVITE)) {
            return;
        }
        if (!mu_db_account_find(arg.c_str(), &target)) {
            MuSession *t = online(arg);
            if (t == nullptr) {
                say(s, "No such player.");
                return;
            }
            target = t->account;
        }
        if (mu_db_int("SELECT COUNT(*) FROM guild_member WHERE account = ?", 1, "i", target.id) != 0) {
            say(s, "They are already in a guild.");
            return;
        }
        mu_db_run("INSERT OR REPLACE INTO guild_invite (guild, account, by, at) VALUES (?, ?, ?, ?)", "iisi", s->guild_id, target.id,
                  s->char_name.c_str(), (int64_t) time(nullptr));
        std::string gname = mu_guild_name(s->guild_id);
        say(s, "You invite " + std::string(target.name) + " to " + gname + ".");
        MuSession *t = by_account(target.id);
        if (t != nullptr) {
            say(t, s->char_name + " invites you to the guild " + gname + ". Type /guild join " + gname + ".");
        }
    } else if (sub == "join") {
        char folded[33];
        mu_fold(arg.c_str(), folded, sizeof(folded));
        int64_t guild = mu_db_int("SELECT g.id FROM guild g JOIN guild_invite i ON i.guild = g.id WHERE g.folded = ? AND i.account = ?", 0,
                                  "si", folded, s->account.id);
        if (guild == 0 || s->guild_id != 0) {
            say(s, s->guild_id != 0 ? "Leave your guild first." : "You have no invitation from that guild.");
            return;
        }
        mu_db_begin();
        mu_db_run("DELETE FROM guild_invite WHERE account = ?", "i", s->account.id);
        int ok = mu_db_run("INSERT INTO guild_member (account, guild, rank, joined) VALUES (?, ?, 4, ?)", "iii", s->account.id, guild,
                           (int64_t) time(nullptr));
        if (ok != 1) {
            mu_db_rollback();
        }
        if (ok != 1 || mu_db_commit() != 0) {
            say(s, "Could not join.");
            return;
        }
        load_guild(s);
        to_guild_members(guild, "[Guild] " + s->char_name + " has joined the guild.");
    } else if (sub == "leave") {
        if (s->guild_id == 0) {
            say(s, "You are not in a guild.");
            return;
        }
        if (s->guild_rank == 0) {
            say(s, "The leader cannot leave: /guild promote someone to Leader, or /guild disband.");
            return;
        }
        mu_db_run("DELETE FROM guild_member WHERE account = ?", "i", s->account.id);
        to_guild_members(s->guild_id, "[Guild] " + s->char_name + " has left the guild.");
        s->guild_id = 0;
    } else if (sub == "kick" || sub == "promote" || sub == "demote") {
        int perm = sub == "kick" ? GP_KICK : sub == "promote" ? GP_PROMOTE : GP_DEMOTE;
        int rank = 0;
        if (!need(s, perm)) {
            return;
        }
        if (!mu_db_account_find(arg.c_str(), &target) || !member_rank(s->guild_id, target.id, rank)) {
            MuSession *t = online(arg);
            if (t == nullptr || t->guild_id != s->guild_id) {
                say(s, "They are not in your guild.");
                return;
            }
            target = t->account;
            rank = t->guild_rank;
        }
        // only on lower ranks; promoting to Leader hands the guild over
        if (rank <= s->guild_rank && !(s->guild_rank == 0 && target.id != s->account.id && sub != "kick")) {
            say(s, "You can only do that to lower ranks.");
            return;
        }
        std::string name = target.name;
        if (sub == "kick") {
            mu_db_run("DELETE FROM guild_member WHERE account = ? AND guild = ?", "ii", target.id, s->guild_id);
            to_guild_members(s->guild_id, "[Guild] " + name + " was removed by " + s->char_name + ".");
        } else {
            int to = sub == "promote" ? rank - 1 : rank + 1;
            if (to < 0 || to > 4 || (sub == "promote" && to < s->guild_rank && s->guild_rank != 0)) {
                say(s, "They cannot go further.");
                return;
            }
            mu_db_begin();
            mu_db_run("UPDATE guild_member SET rank = ? WHERE account = ? AND guild = ?", "iii", (int64_t) to, target.id, s->guild_id);
            if (to == 0) {
                // a new leader: the old one becomes an officer
                mu_db_run("UPDATE guild_member SET rank = 1 WHERE account = ? AND guild = ?", "ii", s->account.id, s->guild_id);
                mu_db_run("UPDATE guild SET leader = ? WHERE id = ?", "ii", target.id, s->guild_id);
            }
            mu_db_commit();
            load_guild(s);
            to_guild_members(s->guild_id, "[Guild] " + name + " is now " + rank_title(s->guild_id, to) + ".");
        }
        refresh_guild(target.id);
    } else if (sub == "motd") {
        if (!need(s, GP_MOTD)) {
            return;
        }
        mu_db_run("UPDATE guild SET motd = ? WHERE id = ?", "si", arg.c_str(), s->guild_id);
        to_guild_members(s->guild_id, "[Guild] Message of the day: " + arg);
    } else if (sub == "rank") {
        // /guild rank <0-4> <title> [perms]
        if (!need(s, GP_RANKS)) {
            return;
        }
        std::vector<std::string> a = words(arg, 3);
        if (a.size() < 2 || a[0].size() != 1 || a[0][0] < '1' || a[0][0] > '4') {
            say(s, "Use: /guild rank <1-4> <title> [permission number]. Rank 0 is the leader's.");
            return;
        }
        int64_t rank = a[0][0] - '0';
        mu_db_run("UPDATE guild_rank SET title = ? WHERE guild = ? AND rank = ?", "sii", a[1].c_str(), s->guild_id, rank);
        if (a.size() > 2) {
            int perms = atoi(a[2].c_str()) & (GP_ALL & ~GP_RANKS);
            mu_db_run("UPDATE guild_rank SET perms = ? WHERE guild = ? AND rank = ?", "iii", (int64_t) perms, s->guild_id, rank);
        }
        say(s, "Rank " + a[0] + " updated.");
    } else if (sub == "turns") {
        turns_command(s, true, lower(arg));
    } else if (sub == "shield") {
        // the colour every member stands on; the party's and the plain one are not offered
        if (arg.empty()) {
            say(s, std::string("Shield: ") + (s->guild_id != 0 ? mu_shield_name(s->guild_shield) : "none") + ". Colours: " + mu_shield_list());
            return;
        }
        if (!need(s, GP_RANKS)) {
            return;
        }
        int colour = mu_shield_by_name(lower(arg));
        if (colour == 0) {
            say(s, "Not a guild colour. Choose from: " + mu_shield_list());
            return;
        }
        mu_db_run("UPDATE guild SET shield = ? WHERE id = ?", "ii", (int64_t) colour, s->guild_id);
        pthread_mutex_lock(&mu_sessions_lock);
        for (MuSession *p : mu_sessions) {
            if (p->guild_id == s->guild_id) {
                p->guild_shield = colour;
            }
        }
        pthread_mutex_unlock(&mu_sessions_lock);
        to_guild_members(s->guild_id, "[Guild] The guild's shield is now " + lower(arg) + ".");
    } else if (sub == "disband") {
        if (s->guild_id == 0 || s->guild_rank != 0) {
            say(s, "Only the leader can disband the guild.");
            return;
        }
        if (mu_db_int("SELECT COUNT(*) FROM guild_vault WHERE guild = ?", 0, "i", s->guild_id) > 0 ||
            mu_db_int("SELECT gold FROM guild WHERE id = ?", 0, "i", s->guild_id) > 0) {
            say(s, "Empty the guild bank first.");
            return;
        }
        int64_t guild = s->guild_id;
        to_guild_members(guild, "[Guild] The guild is disbanded.");
        mu_db_begin();
        mu_db_run("DELETE FROM guild_member WHERE guild = ?", "i", guild);
        mu_db_run("DELETE FROM guild_rank WHERE guild = ?", "i", guild);
        mu_db_run("DELETE FROM guild_invite WHERE guild = ?", "i", guild);
        mu_db_run("DELETE FROM guild WHERE id = ?", "i", guild);
        mu_db_commit();
        pthread_mutex_lock(&mu_sessions_lock);
        for (MuSession *p : mu_sessions) {
            if (p->guild_id == guild) {
                p->guild_id = 0;
            }
        }
        pthread_mutex_unlock(&mu_sessions_lock);
        mu_db_audit(s->account.name, "guild-disband", std::to_string(guild).c_str(), "");
    } else if (sub == "members") {
        if (s->guild_id == 0) {
            say(s, "You are not in a guild.");
            return;
        }
        std::string line = "Members:";
        MuRows *r = mu_db_query("SELECT a.name, r.title FROM guild_member m JOIN account a ON a.id = m.account "
                                "JOIN guild_rank r ON r.guild = m.guild AND r.rank = m.rank WHERE m.guild = ? ORDER BY m.rank, a.name",
                                "i", s->guild_id);
        while (mu_db_next(r)) {
            std::string add = std::string(" ") + mu_db_col_text(r, 0) + " (" + mu_db_col_text(r, 1) + ")";
            if (line.size() + add.size() > 76) {
                say(s, line);
                line = " ";
            }
            line += add;
        }
        mu_db_done(r);
        say(s, line);
    } else {
        if (s->guild_id == 0) {
            say(s, "Not in a guild. /guild create <name> (" + std::to_string(mu_guild_cost) + " gold), or /guild join <name>.");
            return;
        }
        MuRows *r = mu_db_query("SELECT name, motd, gold FROM guild WHERE id = ?", "i", s->guild_id);
        if (mu_db_next(r)) {
            say(s, std::string(mu_db_col_text(r, 0)) + " - you are " + rank_title(s->guild_id, s->guild_rank) + ". Bank: " +
                       std::to_string(mu_db_col_int(r, 2)) + " gold.");
            if (*mu_db_col_text(r, 1) != '\0') {
                say(s, std::string("MOTD: ") + mu_db_col_text(r, 1));
            }
        }
        mu_db_done(r);
        say(s, "/guild invite|join|leave|kick|promote|demote|motd|rank|shield|members|disband");
    }
}

// --- moderation ---------------------------------------------------------------

static std::string describe(MuSession *t) {
    Player_t const &p = (t == mu_self) ? py : t->ctx.py;
    char where[32];
    return t->char_name + " [" + t->account.name + "] level " + std::to_string(p.misc.level) + " " + classes[p.misc.class_id].title +
           ", " + mu_depth_name(t->level != nullptr ? t->level->depth : 0, where, sizeof(where)) + ", HP " + std::to_string(p.misc.current_hp) +
           "/" + std::to_string(p.misc.max_hp) + ", " + std::to_string(p.misc.au) + " gold";
}

static void who(MuSession *s) {
    std::vector<std::string> lines;
    char where[32];
    for (MuSession *p : mu_players) {
        Player_t const &pp = (p == s) ? py : p->ctx.py;
        std::string line = p->char_name + " (" + std::to_string(pp.misc.level) + " " + classes[pp.misc.class_id].title + ", " +
                           mu_depth_name(p->level != nullptr ? p->level->depth : 0, where, sizeof(where)) + ")";
        if (s->account.role >= 1) {
            line += " [" + std::string(p->account.name) + "]";
        }
        lines.push_back(line);
    }
    say(s, std::to_string(lines.size()) + " in the game:");
    std::string line;
    for (auto const &l : lines) {
        if (line.size() + l.size() + 2 > 78) {
            say(s, line);
            line.clear();
        }
        line += (line.empty() ? "" : ", ") + l;
    }
    if (!line.empty()) {
        say(s, line);
    }
}

static void move_beside(MuSession *t) {
    // /goto on the same level: Umoria's playerTeleport(), to a chosen square
    Coord_t spot = t->ctx.py.pos;
    muFindSpot(spot);
    dungeonMoveCreatureRecord(py.pos, spot);
    Coord_t c;
    for (c.y = py.pos.y - 1; c.y <= py.pos.y + 1; c.y++) {
        for (c.x = py.pos.x - 1; c.x <= py.pos.x + 1; c.x++) {
            dg.floor[c.y][c.x].temporary_light = false;
            dungeonLiteSpot(c);
        }
    }
    dungeonLiteSpot(py.pos);
    py.pos = spot;
    dungeonResetView();
    updateMonsters(false);
}

static bool moderate(MuSession *s, const std::string &cmd, const std::vector<std::string> &w) {
    int role = s->account.role;
    std::string arg = w.size() > 1 ? w[1] : "";
    std::string rest = w.size() > 2 ? w[2] : "";
    MuAccount target{};
    time_t now = time(nullptr);

    if (cmd == "kick" && role >= 1) {
        MuSession *t = online(arg);
        if (t == nullptr) {
            say(s, "Not online.");
            return true;
        }
        t->kicked = true;
        t->kick_reason = rest.empty() ? "kicked by a moderator" : "kicked: " + rest;
        mu_conn_kill(t->conn);
        mu_db_audit(s->account.name, "kick", t->account.name, rest.c_str());
        say(s, "Kicked " + t->char_name + ".");
    } else if (cmd == "mute" && role >= 1) {
        if (!mu_db_account_find(arg.c_str(), &target)) {
            MuSession *t = online(arg);
            if (t == nullptr) {
                say(s, "No such account.");
                return true;
            }
            target = t->account;
        }
        int minutes = rest.empty() ? 10 : atoi(rest.c_str());
        mu_db_run("UPDATE account SET muted_until = ? WHERE id = ?", "ii", (int64_t) (now + minutes * 60), target.id);
        MuSession *t = by_account(target.id);
        if (t != nullptr) {
            t->account.muted_until = now + minutes * 60;
            say(t, "You have been muted for " + std::to_string(minutes) + " minutes.");
        }
        mu_db_audit(s->account.name, "mute", target.name, std::to_string(minutes).c_str());
        say(s, "Muted " + std::string(target.name) + " for " + std::to_string(minutes) + " minutes.");
    } else if (cmd == "goto" && role >= 1) {
        MuSession *t = online(arg);
        if (t == nullptr || !t->in_game || t == s) {
            say(s, "Not in the game.");
            return true;
        }
        if (t->level == s->level) {
            move_beside(t);
        } else {
            s->arrive_near = t;
            mu_send_to_depth(t->level->depth);
        }
        mu_db_audit(s->account.name, "goto", t->char_name.c_str(), "");
    } else if (cmd == "ban" && role >= 2) {
        // /ban name days reason
        std::vector<std::string> a = words(rest, 2);
        int days = a.empty() ? 1 : atoi(a[0].c_str());
        std::string reason = a.size() > 1 ? a[1] : "banned";
        if (!mu_db_account_find(arg.c_str(), &target)) {
            say(s, "No such account.");
            return true;
        }
        mu_db_run("UPDATE account SET banned_until = ?, ban_reason = ? WHERE id = ?", "isi", (int64_t) (now + (time_t) days * 86400),
                  reason.c_str(), target.id);
        MuSession *t = by_account(target.id);
        if (t != nullptr) {
            t->kicked = true;
            t->kick_reason = "banned: " + reason;
            mu_conn_kill(t->conn);
        }
        mu_db_audit(s->account.name, "ban", target.name, (std::to_string(days) + " days: " + reason).c_str());
        say(s, "Banned " + std::string(target.name) + " for " + std::to_string(days) + " days.");
    } else if (cmd == "unban" && role >= 2) {
        if (mu_db_run("UPDATE account SET banned_until = 0, ban_reason = '' WHERE folded = lower(replace(?, ' ', ''))", "s", arg.c_str()) == 1) {
            mu_db_audit(s->account.name, "unban", arg.c_str(), "");
            say(s, "Unbanned " + arg + ".");
        } else {
            say(s, "No such account.");
        }
    } else if (cmd == "role" && role >= 2) {
        int r = atoi(rest.c_str());
        if (r < 0 || r > 2 || !mu_db_account_find(arg.c_str(), &target)) {
            say(s, "Use: /role <account> <0 player, 1 moderator, 2 admin>");
            return true;
        }
        mu_db_run("UPDATE account SET role = ? WHERE id = ?", "ii", (int64_t) r, target.id);
        MuSession *t = by_account(target.id);
        if (t != nullptr) {
            t->account.role = r;
        }
        mu_db_audit(s->account.name, "role", target.name, rest.c_str());
        say(s, std::string(target.name) + " now has role " + std::to_string(r) + ".");
    } else if (cmd == "broadcast" && role >= 2) {
        std::string text = arg + (rest.empty() ? "" : " " + rest);
        mu_system_all("*** " + text + " ***");
        mu_db_audit(s->account.name, "broadcast", "", text.c_str());
    } else if (cmd == "summon" && role >= 2) {
        MuSession *t = online(arg);
        if (t == nullptr || !t->in_game || t == s) {
            say(s, "Not in the game.");
            return true;
        }
        // they go at their next turn; the event wakes them for it
        t->arrive_near = s;
        t->pending_depth = s->level->depth;
        t->ctx.generate_new_level = true;
        t->event_pending = true;
        mu_conn_poke(t->conn);
        say(t, "You are summoned by " + s->char_name + ".");
        mu_db_audit(s->account.name, "summon", t->char_name.c_str(), "");
    } else if (cmd == "inspect" && role >= 1) {
        MuSession *t = online(arg);
        if (t != nullptr && t->in_game) {
            say(s, describe(t));
        } else {
            MuRows *r = mu_db_query("SELECT c.name, c.level, c.depth, a.name FROM character c JOIN account a ON a.id = c.account "
                                    "WHERE c.folded = lower(replace(?, ' ', '')) AND c.alive = 1",
                                    "s", arg.c_str());
            if (mu_db_next(r)) {
                say(s, std::string(mu_db_col_text(r, 0)) + " [" + mu_db_col_text(r, 3) + "] level " + std::to_string(mu_db_col_int(r, 1)) +
                           ", " + std::to_string(mu_db_col_int(r, 2) * 50) + " ft (offline)");
            } else {
                say(s, "No such character.");
            }
            mu_db_done(r);
        }
    } else if (cmd == "save" && role >= 2) {
        for (MuSession *p : mu_players) {
            mu_with_player(s, p, [&] { mu_char_store(p); });
        }
        mu_world_maintain(true);
        mu_db_audit(s->account.name, "save", "", "");
        say(s, "The world is saved.");
    } else if (cmd == "shutdown" && role >= 2) {
        int minutes = arg.empty() ? 5 : atoi(arg.c_str());
        mu_shutdown_at = now + minutes * 60;
        mu_system_all("*** The server will shut down in " + std::to_string(minutes) + " minutes. ***");
        mu_db_audit(s->account.name, "shutdown", "", std::to_string(minutes).c_str());
    } else if (cmd == "spawnboss" && role >= 2) {
        say(s, mu_boss_spawn_beside(s->level) ? "A boss appears beside you." : "No boss here (not in the town, and only beside open floor).");
        mu_db_audit(s->account.name, "spawnboss", std::to_string(s->level->depth).c_str(), "");
    } else if (cmd == "resetlevel" && role >= 2) {
        int depth = atoi(arg.c_str());
        if (depth <= 0) {
            say(s, "Use: /resetlevel <depth in levels, 1 or more>");
            return true;
        }
        mu_forget_level(depth);
        mu_db_audit(s->account.name, "resetlevel", arg.c_str(), "");
        say(s, "Level " + arg + " will be made anew when nobody is on it.");
    } else {
        return false;
    }
    return true;
}

static void help(MuSession *s) {
    say(s, "' then text: say to your level. /w world, /g guild, /p party, /t name msg, /r reply");
    say(s, "/who  /invite /accept /decline /leave /party /pkick  /trade name  /guild ...  /pvp");
    say(s, "\" then a direction: attack a player (both need PvP on; not in the town).");
    say(s, "Town: 7 Bank, 8 Guild Hall, 9 Auction House.");
    if (s->account.role >= 1) {
        say(s, "Mod: /kick /mute /goto /inspect" + std::string(s->account.role >= 2 ? "  Admin: /ban /unban /role /broadcast /summon /spawnboss /save /shutdown /resetlevel" : ""));
    }
}

void mu_social_command(MuSession *s, const std::string &line) {
    if (line.empty()) {
        return;
    }
    if (line[0] != '/') {
        if (may_chat(s)) {
            to_level(s, line.substr(0, MU_CHAT_MAX_LEN));
        }
        return;
    }
    std::vector<std::string> w = words(line.substr(1), 3);
    if (w.empty()) {
        return;
    }
    std::string cmd = lower(w[0]);
    std::string rest = w.size() > 1 ? w[1] + (w.size() > 2 ? " " + w[2] : "") : "";

    if (cmd == "w" || cmd == "g" || cmd == "p" || cmd == "say" || cmd == "s") {
        if (rest.empty() || !may_chat(s)) {
            return;
        }
        rest = rest.substr(0, MU_CHAT_MAX_LEN);
        if (cmd == "w") {
            to_world(s, rest);
        } else if (cmd == "g") {
            to_guild(s, rest);
        } else if (cmd == "p") {
            to_party(s, rest);
        } else {
            to_level(s, rest);
        }
    } else if (cmd == "t" || cmd == "tell") {
        if (w.size() < 3 || !may_chat(s)) {
            say(s, "Use: /t name message");
            return;
        }
        tell(s, w[1], w[2].substr(0, MU_CHAT_MAX_LEN));
    } else if (cmd == "r") {
        if (s->reply_to.empty() || rest.empty() || !may_chat(s)) {
            say(s, "Nobody to reply to.");
            return;
        }
        tell(s, s->reply_to, rest.substr(0, MU_CHAT_MAX_LEN));
    } else if (cmd == "who") {
        who(s);
    } else if (cmd == "invite") {
        party_invite(s, rest);
    } else if (cmd == "accept") {
        party_accept(s);
    } else if (cmd == "decline") {
        MuSession *inviter = s->invited_by != 0 ? by_account(s->invited_by) : nullptr;
        s->invited_by = 0;
        if (inviter != nullptr) {
            say(inviter, s->char_name + " declines your invitation.");
        }
    } else if (cmd == "leave") {
        party_drop(s);
    } else if (cmd == "party") {
        if (lower(w.size() > 1 ? w[1] : "") == "turns") {
            turns_command(s, false, lower(w.size() > 2 ? w[2] : ""));
        } else {
            party_list(s);
        }
    } else if (cmd == "pkick") {
        party_kick(s, rest);
    } else if (cmd == "guild") {
        guild_command(s, words(line.substr(1), 3));
    } else if (cmd == "pvp") {
        pvp_command(s, lower(rest));
    } else if (cmd == "trade") {
        mu_trade_request(s, rest);
    } else if (cmd == "help" || cmd == "?") {
        help(s);
    } else if (!moderate(s, cmd, w)) {
        say(s, "Unknown command. /help lists them.");
    }
    if (cmd == "accept" || cmd == "leave" || cmd == "pkick" || cmd == "guild") {
        restyle_everyone();
    }
}

// --- arriving and leaving -------------------------------------------------------

void mu_social_entered(MuSession *s) {
    load_guild(s);
    say(s, "Welcome to Mumoria. Press ' to talk; /help for commands.");
    if (s->guild_id != 0) {
        std::string motd;
        MuRows *r = mu_db_query("SELECT motd FROM guild WHERE id = ?", "i", s->guild_id);
        if (mu_db_next(r)) {
            motd = mu_db_col_text(r, 0);
        }
        mu_db_done(r);
        if (!motd.empty()) {
            say(s, "[Guild] " + motd);
        }
        to_guild_members(s->guild_id, "[Guild] " + s->char_name + " has come online.");
    }
    int64_t claims = mu_db_int("SELECT COUNT(*) FROM claim WHERE account = ?", 0, "i", s->account.id);
    if (claims > 0) {
        say(s, "The Auction House holds " + std::to_string(claims) + " thing(s) for you.");
    }
}

void mu_social_left(MuSession *s, bool died) {
    mu_trade_cancel(s);
    if (s->party != nullptr) {
        party_drop(s);
        restyle_everyone();
    }
    if (s->guild_id != 0 && !died) {
        to_guild_members(s->guild_id, "[Guild] " + s->char_name + " has gone offline.");
    }
}

// --- the extra key --------------------------------------------------------------

bool muCommand(char key) {
    MuSession *s = mu_self;
    if ((unsigned char) key == MU_KEY_EVENT) {
        s->event_pending = false;
        game.player_free_turn = true;
        if (s->pending_pvp_damage > 0) {
            int damage = s->pending_pvp_damage;
            s->pending_pvp_damage = 0;
            playerDisturb(1, 0);
            printMessage((s->pvp_attacker + " hits you.").c_str());
            playerTakesHit(damage, s->pvp_attacker.c_str()); // a death ends the turn loop
        }
        if (s->trade != nullptr && !game.character_is_dead) {
            mu_trade_screen(s);
        }
        return true;
    }
    if (key == '"') {
        pvp_attack(s);
        return true;
    }
    if (key != '\'') {
        return false;
    }
    game.player_free_turn = true;
    MuSafePrompt safe;
    putStringClearToEOL(">", Coord_t{0, 0});
    char buf[80] = {0};
    bool typed = getStringInput(buf, Coord_t{0, 2}, 77);
    messageLineClear();
    if (typed) {
        mu_social_command(s, buf);
    }
    return true;
}
