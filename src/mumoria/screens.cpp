// Mumoria - the Bank, Guild Hall, Auction House and Trade screens.
//
// THE ANTI-DUPLICATION RULE (docs/MUMORIA_DESIGN.md 2.14): an item or gold
// moving between a character and the database goes through econ(): one
// transaction holds the row change AND the character's save. A crash keeps
// both or neither, so nothing can be in two places. The world lock (held
// here) keeps two such moves from interleaving; contested rows are changed
// by conditional UPDATE/DELETE whose row count is checked.

#include "screens.h"

#include "mu_config.h"
#include "social.h"

// --- drawing, as store.cpp does --------------------------------------------------

struct Row {
    std::string text, right;
};

static const int PAGE = 12;

static void draw_page(const std::string &title, const std::string &info1, const std::string &info2, std::vector<Row> const &rows, int top,
                      std::vector<std::string> const &commands) {
    clearScreen();
    putString(title.c_str(), Coord_t{3, 9});
    putString(info1.c_str(), Coord_t{3, 50});
    putString(info2.c_str(), Coord_t{4, 50});
    if (rows.empty()) {
        putString("(nothing)", Coord_t{6, 3});
    }
    for (int i = 0; i < PAGE && top + i < (int) rows.size(); i++) {
        char line[90];
        snprintf(line, sizeof(line), "%c) %s", 'a' + i, rows[(size_t) (top + i)].text.c_str());
        putString(line, Coord_t{5 + i, 0});
        putString(rows[(size_t) (top + i)].right.c_str(), Coord_t{5 + i, 58});
    }
    if ((int) rows.size() > PAGE) {
        char pages[40];
        snprintf(pages, sizeof(pages), "- page %d of %d -", top / PAGE + 1, ((int) rows.size() + PAGE - 1) / PAGE);
        putString(pages, Coord_t{17, 30});
    }
    for (size_t i = 0; i < commands.size(); i++) {
        putString(commands[i].c_str(), Coord_t{19 + (int) i / 2, (int) (i % 2) * 40});
    }
}

static void page_keys(char cmd, int &top, int count) {
    if (cmd == '+' || cmd == ' ') {
        top = (top + PAGE < count) ? top + PAGE : 0;
    } else if (cmd == '-') {
        top = (top >= PAGE) ? top - PAGE : 0;
    }
}

// The row a letter picks on this page, or -1.
static int pick_row(const char *prompt, int top, int count) {
    char which;
    if (count == 0 || !getCommand(prompt, which)) {
        return -1;
    }
    int i = top + (which - 'a');
    if (which < 'a' || which >= 'a' + PAGE || i >= count) {
        printMessage("There is nothing there.");
        return -1;
    }
    return i;
}

static bool ask_number(const char *prompt, int64_t &out) {
    putStringClearToEOL(prompt, Coord_t{0, 0});
    char buf[16] = {0};
    if (!getStringInput(buf, Coord_t{0, (int) strlen(prompt) + 1}, 10)) {
        return false;
    }
    char *end = nullptr;
    long long v = strtoll(buf, &end, 10);
    if (end == buf || v < 0) {
        printMessage("That is not a number.");
        return false;
    }
    out = v;
    return true;
}

static std::string describe(Inventory_t const &item) {
    obj_desc_t d;
    itemDescription(d, item, true);
    std::string text = d;
    if (!text.empty() && text.back() == '.') {
        text.pop_back(); // Umoria ends the description with a full stop
    }
    return text;
}

static std::string gold_text(int64_t gold) {
    return std::to_string(gold) + " gold";
}

// --- moving things, safely --------------------------------------------------------

template <typename F> static bool econ(MuSession *s, F change) {
    Player_t before = py;
    if (mu_db_begin() != 0) {
        printMessage("The clerk is busy. Try again.");
        return false;
    }
    bool ok = change();
    ok = ok && mu_char_store(s);
    if (ok) {
        ok = mu_db_commit() == 0; // a failed commit rolls back
    } else {
        mu_db_rollback();
    }
    if (!ok) {
        py = before;
    }
    return ok;
}

// Takes a whole pack slot (the stack, not one of it).
static Inventory_t take_slot(int slot) {
    Inventory_t item = py.inventory[slot];
    py.pack.weight -= item.weight * item.items_count;
    for (int i = slot; i < py.pack.unique_items - 1; i++) {
        py.inventory[i] = py.inventory[i + 1];
    }
    inventoryItemCopyTo(config::dungeon::objects::OBJ_NOTHING, py.inventory[py.pack.unique_items - 1]);
    py.pack.unique_items--;
    py.flags.status |= config::player::status::PY_STR_WGT;
    return item;
}

static bool item_from_blob(MuRows *r, int col, Inventory_t &item) {
    int len = 0;
    const void *blob = mu_db_col_blob(r, col, &len);
    if (blob == nullptr || len != (int) sizeof(Inventory_t)) {
        return false;
    }
    memcpy(&item, blob, sizeof(item));
    return true;
}

static bool pick_pack_item(const char *prompt, int &slot) {
    if (py.pack.unique_items == 0) {
        printMessage("You are not carrying anything.");
        return false;
    }
    return inventoryGetInputForItemId(slot, prompt, 0, py.pack.unique_items - 1, nullptr, nullptr);
}

static void bank_add(int64_t account, int64_t gold) {
    mu_db_run("INSERT OR IGNORE INTO bank (account, gold) VALUES (?, 0)", "i", account);
    mu_db_run("UPDATE bank SET gold = gold + ? WHERE account = ?", "ii", gold, account);
}

struct Stored {
    int64_t id;
    std::string text;
    Inventory_t item;
};

static std::vector<Stored> stored(const char *sql, int64_t owner) {
    std::vector<Stored> out;
    MuRows *r = mu_db_query(sql, "i", owner);
    while (mu_db_next(r)) {
        Stored st{mu_db_col_int(r, 0), mu_db_col_text(r, 1), {}};
        if (item_from_blob(r, 2, st.item)) {
            out.push_back(st);
        }
    }
    mu_db_done(r);
    return out;
}

// --- the Bank (7) ---------------------------------------------------------------

static void bank_screen(MuSession *s) {
    int top = 0;
    for (;;) {
        int64_t gold = mu_db_int("SELECT gold FROM bank WHERE account = ?", 0, "i", s->account.id);
        std::vector<Stored> items = stored("SELECT id, item, blob FROM bank_item WHERE account = ? ORDER BY id", s->account.id);
        std::vector<Row> rows;
        for (auto const &st : items) {
            rows.push_back(Row{st.text, ""});
        }
        draw_page("The Bank of Mumoria", "Carried: " + gold_text(py.misc.au), "In bank: " + gold_text(gold), rows, top,
                  {"d) Deposit gold", "w) Withdraw gold", "s) Store an item", "r) Retrieve an item", "+/-) Page", "ESC) Leave"});
        putString(("Vault: " + std::to_string(items.size()) + " of " + std::to_string(MU_BANK_SLOTS)).c_str(), Coord_t{17, 58});

        char cmd;
        if (!getCommand("", cmd)) {
            return;
        }
        int64_t amount = 0;
        int slot = 0;
        switch (cmd) {
            case 'd':
                if (ask_number("Deposit how much gold?", amount) && amount > 0) {
                    if (amount > py.misc.au) {
                        printMessage("You do not have that much.");
                    } else if (econ(s, [&] {
                                   py.misc.au -= (int32_t) amount;
                                   bank_add(s->account.id, amount);
                                   return true;
                               })) {
                        mu_db_ledger(s->account.id, s->char_name.c_str(), "bank-deposit", amount, "", "");
                    }
                }
                break;
            case 'w':
                if (ask_number("Withdraw how much gold?", amount) && amount > 0) {
                    if (!econ(s, [&] {
                            if (mu_db_run("UPDATE bank SET gold = gold - ? WHERE account = ? AND gold >= ?", "iii", amount, s->account.id,
                                          amount) != 1) {
                                return false;
                            }
                            py.misc.au += (int32_t) amount;
                            return true;
                        })) {
                        printMessage("The bank does not hold that much for you.");
                    } else {
                        mu_db_ledger(s->account.id, s->char_name.c_str(), "bank-withdraw", -amount, "", "");
                    }
                }
                break;
            case 's':
                if ((int) items.size() >= MU_BANK_SLOTS) {
                    printMessage("Your vault is full.");
                } else if (pick_pack_item("Store which item?", slot)) {
                    std::string text;
                    if (econ(s, [&] {
                        Inventory_t item = take_slot(slot);
                        text = describe(item);
                        return mu_db_run("INSERT INTO bank_item (account, blob, item, at) VALUES (?, ?, ?, ?)", "ibsi", s->account.id, &item,
                                         (int) sizeof(item), text.c_str(), (int64_t) time(nullptr)) == 1;
                    })) {
                        mu_db_ledger(s->account.id, s->char_name.c_str(), "bank-store", 0, text.c_str(), "");
                    }
                }
                break;
            case 'r': {
                int i = pick_row("Retrieve which item?", top, (int) items.size());
                if (i < 0) {
                    break;
                }
                Stored const &st = items[(size_t) i];
                if (!inventoryCanCarryItemCount(st.item)) {
                    printMessage("You cannot carry that many items.");
                    break;
                }
                if (econ(s, [&] {
                        if (mu_db_run("DELETE FROM bank_item WHERE id = ? AND account = ?", "ii", st.id, s->account.id) != 1) {
                            return false;
                        }
                        Inventory_t item = st.item;
                        inventoryCarryItem(item);
                        return true;
                    })) {
                    mu_db_ledger(s->account.id, s->char_name.c_str(), "bank-retrieve", 0, st.text.c_str(), "");
                }
                break;
            }
            default:
                page_keys(cmd, top, (int) items.size());
                break;
        }
    }
}

// --- the Guild Hall (8) -----------------------------------------------------------

static void guild_screen(MuSession *s) {
    int top = 0;
    for (;;) {
        if (s->guild_id == 0) {
            clearScreen();
            putString("The Guild Hall", Coord_t{3, 9});
            putString("You belong to no guild.", Coord_t{6, 3});
            putString(("A clerk can register a new one for " + gold_text(mu_guild_cost) + ".").c_str(), Coord_t{7, 3});
            putString("Or ask a guild for an invitation, then type /guild join <name>.", Coord_t{8, 3});
            putString("c) Found a guild", Coord_t{19, 0});
            putString("ESC) Leave", Coord_t{19, 40});
            char cmd;
            if (!getCommand("", cmd)) {
                return;
            }
            if (cmd == 'c') {
                putStringClearToEOL("Name of the new guild:", Coord_t{0, 0});
                char buf[32] = {0};
                if (getStringInput(buf, Coord_t{0, 23}, 24)) {
                    std::string why;
                    if (mu_guild_create(s, buf, why)) {
                        printMessage(("You found " + std::string(buf) + ".").c_str());
                    } else {
                        printMessage(why.c_str());
                    }
                }
            }
            continue;
        }

        int perms = mu_guild_perms(s->guild_id, s->guild_rank);
        int64_t gold = mu_db_int("SELECT gold FROM guild WHERE id = ?", 0, "i", s->guild_id);
        std::vector<Stored> items = stored("SELECT id, item, blob FROM guild_vault WHERE guild = ? ORDER BY id", s->guild_id);
        std::vector<Row> rows;
        for (auto const &st : items) {
            rows.push_back(Row{st.text, ""});
        }
        draw_page("The Guild Hall: " + mu_guild_name(s->guild_id), "Carried: " + gold_text(py.misc.au), "Guild bank: " + gold_text(gold), rows,
                  top, {"d) Deposit gold", "w) Withdraw gold", "s) Store an item", "r) Retrieve an item", "+/-) Page", "ESC) Leave"});

        char cmd;
        if (!getCommand("", cmd)) {
            return;
        }
        int64_t amount = 0;
        int slot = 0;
        bool may_put = (perms & GP_DEPOSIT) != 0, may_take = (perms & GP_WITHDRAW) != 0;
        switch (cmd) {
            case 'd':
                if (!may_put) {
                    printMessage("Your rank may not use the guild bank.");
                } else if (ask_number("Deposit how much gold?", amount) && amount > 0 && amount <= py.misc.au) {
                    if (econ(s, [&] {
                        py.misc.au -= (int32_t) amount;
                        return mu_db_run("UPDATE guild SET gold = gold + ? WHERE id = ?", "ii", amount, s->guild_id) == 1;
                    })) {
                        mu_db_ledger(s->account.id, s->char_name.c_str(), "guild-deposit", amount, "", std::to_string(s->guild_id).c_str());
                    }
                }
                break;
            case 'w':
                if (!may_take) {
                    printMessage("Your rank may not take from the guild bank.");
                } else if (ask_number("Withdraw how much gold?", amount) && amount > 0) {
                    if (econ(s, [&] {
                            if (mu_db_run("UPDATE guild SET gold = gold - ? WHERE id = ? AND gold >= ?", "iii", amount, s->guild_id, amount) != 1) {
                                return false;
                            }
                            py.misc.au += (int32_t) amount;
                            return true;
                        })) {
                        mu_db_ledger(s->account.id, s->char_name.c_str(), "guild-withdraw", -amount, "", std::to_string(s->guild_id).c_str());
                    } else {
                        printMessage("The guild bank does not hold that much.");
                    }
                }
                break;
            case 's':
                if (!may_put) {
                    printMessage("Your rank may not use the guild bank.");
                } else if ((int) items.size() >= MU_GUILD_VAULT_SLOTS) {
                    printMessage("The guild vault is full.");
                } else if (pick_pack_item("Store which item?", slot)) {
                    std::string text;
                    if (econ(s, [&] {
                        Inventory_t item = take_slot(slot);
                        text = describe(item);
                        return mu_db_run("INSERT INTO guild_vault (guild, blob, item, deposited_by, at) VALUES (?, ?, ?, ?, ?)", "ibssi", s->guild_id,
                                         &item, (int) sizeof(item), text.c_str(), s->char_name.c_str(), (int64_t) time(nullptr)) == 1;
                    })) {
                        mu_db_ledger(s->account.id, s->char_name.c_str(), "guild-store", 0, text.c_str(), std::to_string(s->guild_id).c_str());
                    }
                }
                break;
            case 'r': {
                if (!may_take) {
                    printMessage("Your rank may not take from the guild vault.");
                    break;
                }
                int i = pick_row("Retrieve which item?", top, (int) items.size());
                if (i < 0) {
                    break;
                }
                Stored const &st = items[(size_t) i];
                if (!inventoryCanCarryItemCount(st.item)) {
                    printMessage("You cannot carry that many items.");
                    break;
                }
                if (econ(s, [&] {
                    if (mu_db_run("DELETE FROM guild_vault WHERE id = ? AND guild = ?", "ii", st.id, s->guild_id) != 1) {
                        return false;
                    }
                    Inventory_t item = st.item;
                    inventoryCarryItem(item);
                    return true;
                })) {
                    mu_db_ledger(s->account.id, s->char_name.c_str(), "guild-retrieve", 0, st.text.c_str(), std::to_string(s->guild_id).c_str());
                }
                break;
            }
            default:
                page_keys(cmd, top, (int) items.size());
                break;
        }
    }
}

// --- the Auction House (9) ---------------------------------------------------------

void mu_auction_settle() {
    struct Ended {
        int64_t id, seller, bidder, bid;
        std::vector<uint8_t> blob;
        std::string item;
    };
    std::vector<Ended> ended;
    MuRows *r = mu_db_query("SELECT id, seller, bidder, high_bid, blob, item FROM auction WHERE state = 0 AND ends_at <= ?", "i",
                            (int64_t) time(nullptr));
    while (mu_db_next(r)) {
        int len = 0;
        const uint8_t *p = (const uint8_t *) mu_db_col_blob(r, 4, &len);
        ended.push_back(Ended{mu_db_col_int(r, 0), mu_db_col_int(r, 1), mu_db_col_int(r, 2), mu_db_col_int(r, 3),
                              std::vector<uint8_t>(p, p + len), mu_db_col_text(r, 5)});
    }
    mu_db_done(r);

    int64_t now = (int64_t) time(nullptr);
    for (Ended const &e : ended) {
        if (mu_db_begin() != 0) {
            return;
        }
        if (mu_db_run("UPDATE auction SET state = 2 WHERE id = ? AND state = 0", "i", e.id) != 1) {
            mu_db_rollback();
            continue;
        }
        bool sold = e.bidder != 0;
        mu_db_run("INSERT INTO claim (account, blob, item, gold, reason, at) VALUES (?, ?, ?, 0, ?, ?)", "ibssi", sold ? e.bidder : e.seller,
                  e.blob.data(), (int) e.blob.size(), e.item.c_str(), sold ? "won" : "unsold", now);
        if (sold) {
            bank_add(e.seller, e.bid * (100 - MU_AH_CUT_PERCENT) / 100);
            mu_db_ledger(e.seller, "auction", "auction-sold", e.bid, e.item.c_str(), std::to_string(e.id).c_str());
        }
        mu_db_commit();
    }
}

struct Lot {
    int64_t id, seller, start, buyout, bid, bidder, ends;
    std::string seller_name, item;
    Inventory_t thing;
};

static std::vector<Lot> lots() {
    std::vector<Lot> out;
    MuRows *r = mu_db_query("SELECT id, seller, start, buyout, high_bid, bidder, ends_at, seller_name, item, blob FROM auction "
                            "WHERE state = 0 ORDER BY ends_at",
                            "");
    while (mu_db_next(r)) {
        Lot l{mu_db_col_int(r, 0), mu_db_col_int(r, 1), mu_db_col_int(r, 2), mu_db_col_int(r, 3), mu_db_col_int(r, 4), mu_db_col_int(r, 5),
              mu_db_col_int(r, 6), mu_db_col_text(r, 7), mu_db_col_text(r, 8), {}};
        if (item_from_blob(r, 9, l.thing)) {
            out.push_back(l);
        }
    }
    mu_db_done(r);
    return out;
}

static std::string time_left(int64_t ends) {
    int64_t left = ends - (int64_t) time(nullptr);
    if (left <= 0) {
        return "ending";
    }
    return left >= 3600 ? std::to_string(left / 3600) + "h" : std::to_string(left / 60 + 1) + "m";
}

static void tell_outbid(int64_t account, const std::string &item) {
    pthread_mutex_lock(&mu_sessions_lock);
    for (MuSession *p : mu_sessions) {
        if (p->account.id == account) {
            mu_vt_chat(&p->conn->screen, ("You were outbid on " + item + "; your gold is in your bank.").c_str());
        }
    }
    pthread_mutex_unlock(&mu_sessions_lock);
}

static void collect_claims(MuSession *s) {
    struct Claim {
        int64_t id, gold;
        std::string item;
        Inventory_t thing;
        bool has_item;
    };
    std::vector<Claim> claims;
    MuRows *r = mu_db_query("SELECT id, gold, item, blob FROM claim WHERE account = ? ORDER BY id", "i", s->account.id);
    while (mu_db_next(r)) {
        Claim c{mu_db_col_int(r, 0), mu_db_col_int(r, 1), mu_db_col_text(r, 2), {}, false};
        c.has_item = item_from_blob(r, 3, c.thing);
        claims.push_back(c);
    }
    mu_db_done(r);
    if (claims.empty()) {
        printMessage("Nothing is waiting for you.");
        return;
    }
    for (Claim const &c : claims) {
        if (c.has_item && !inventoryCanCarryItemCount(c.thing)) {
            printMessage("You cannot carry any more; the rest waits here.");
            return;
        }
        if (econ(s, [&] {
                if (mu_db_run("DELETE FROM claim WHERE id = ? AND account = ?", "ii", c.id, s->account.id) != 1) {
                    return false;
                }
                py.misc.au += (int32_t) c.gold;
                if (c.has_item) {
                    Inventory_t item = c.thing;
                    inventoryCarryItem(item);
                }
                return true;
            })) {
            mu_db_ledger(s->account.id, s->char_name.c_str(), "claim", c.gold, c.item.c_str(), "");
            printMessage(("You collect " + (c.has_item ? c.item : gold_text(c.gold)) + ".").c_str());
        }
    }
}

static void auction_sell(MuSession *s) {
    if (mu_db_int("SELECT COUNT(*) FROM auction WHERE seller = ? AND state = 0", 0, "i", s->account.id) >= MU_AH_MAX_PER_SELLER) {
        printMessage("You have as many lots up as the house allows.");
        return;
    }
    int slot = 0;
    int64_t start = 0, buyout = 0;
    if (!pick_pack_item("Auction which item?", slot) || !ask_number("Starting bid?", start) || start < 1 ||
        !ask_number("Buyout price (0 for none)?", buyout)) {
        return;
    }
    if (buyout != 0 && buyout < start) {
        printMessage("The buyout must be at least the starting bid.");
        return;
    }
    char hours;
    if (!getCommand("Run it for a) 8 hours, b) 24 hours, c) 48 hours?", hours) || hours < 'a' || hours > 'c') {
        return;
    }
    int64_t ends = (int64_t) time(nullptr) + (hours == 'a' ? 8 : hours == 'b' ? 24 : 48) * 3600;
    std::string text;
    if (econ(s, [&] {
            Inventory_t item = take_slot(slot);
            text = describe(item);
            return mu_db_run("INSERT INTO auction (seller, seller_name, blob, item, start, buyout, ends_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                             "isbsiii", s->account.id, s->char_name.c_str(), &item, (int) sizeof(item), text.c_str(), start, buyout, ends) == 1;
        })) {
        mu_db_ledger(s->account.id, s->char_name.c_str(), "auction-list", 0, text.c_str(), "");
        printMessage("Your lot is up for auction.");
    }
}

static void auction_bid(MuSession *s, Lot const &l, bool buy_out) {
    if (l.seller == s->account.id) {
        printMessage("You cannot bid on your own lot.");
        return;
    }
    int64_t amount = l.buyout;
    if (!buy_out) {
        int64_t least = l.bid > 0 ? l.bid + std::max<int64_t>(1, l.bid / 20) : l.start;
        std::string prompt = "Your bid (at least " + std::to_string(least) + ")?";
        if (!ask_number(prompt.c_str(), amount)) {
            return;
        }
        if (amount < least) {
            printMessage("That is not enough.");
            return;
        }
    } else if (l.buyout <= 0) {
        printMessage("That lot has no buyout price.");
        return;
    }
    if (amount > py.misc.au) {
        printMessage("You do not carry that much gold.");
        return;
    }
    int64_t now = (int64_t) time(nullptr);
    bool delivered = false;
    bool ok = econ(s, [&] {
        int changed;
        if (buy_out) {
            changed = mu_db_run("UPDATE auction SET state = 1, high_bid = ?, bidder = ?, bidder_name = ? "
                                "WHERE id = ? AND state = 0 AND high_bid = ? AND bidder = ? AND ends_at > ?",
                                "iisiiii", amount, s->account.id, s->char_name.c_str(), l.id, l.bid, l.bidder, now);
        } else {
            changed = mu_db_run("UPDATE auction SET high_bid = ?, bidder = ?, bidder_name = ? "
                                "WHERE id = ? AND state = 0 AND high_bid = ? AND bidder = ? AND ends_at > ?",
                                "iisiiii", amount, s->account.id, s->char_name.c_str(), l.id, l.bid, l.bidder, now);
        }
        if (changed != 1) {
            return false; // someone bid first, or it ended
        }
        py.misc.au -= (int32_t) amount;
        if (l.bidder != 0) {
            bank_add(l.bidder, l.bid);
        }
        if (buy_out) {
            bank_add(l.seller, amount * (100 - MU_AH_CUT_PERCENT) / 100);
            if (inventoryCanCarryItemCount(l.thing)) {
                Inventory_t item = l.thing;
                inventoryCarryItem(item);
                delivered = true;
            } else {
                mu_db_run("INSERT INTO claim (account, blob, item, gold, reason, at) VALUES (?, ?, ?, 0, 'bought', ?)", "ibsi", s->account.id,
                          &l.thing, (int) sizeof(l.thing), l.item.c_str(), now);
            }
        }
        return true;
    });
    if (!ok) {
        printMessage("Someone was quicker, or the auction has ended.");
        return;
    }
    if (l.bidder != 0 && l.bidder != s->account.id) {
        tell_outbid(l.bidder, l.item);
    }
    mu_db_ledger(s->account.id, s->char_name.c_str(), buy_out ? "auction-buyout" : "auction-bid", -amount, l.item.c_str(),
                 std::to_string(l.id).c_str());
    printMessage(buy_out ? (delivered ? "It is yours." : "It is yours; collect it here when you have room.") : "Your bid stands.");
}

static void auction_screen(MuSession *s) {
    int top = 0;
    for (;;) {
        mu_auction_settle();
        std::vector<Lot> all = lots();
        std::vector<Row> rows;
        for (Lot const &l : all) {
            std::string right = (l.bid > 0 ? "bid " + std::to_string(l.bid) : "min " + std::to_string(l.start)) +
                                (l.buyout > 0 ? " buy " + std::to_string(l.buyout) : "") + " " + time_left(l.ends);
            std::string text = l.item.substr(0, 50);
            if (l.bidder == s->account.id) {
                text += " *";
            }
            rows.push_back(Row{text, right});
        }
        int64_t claims = mu_db_int("SELECT COUNT(*) FROM claim WHERE account = ?", 0, "i", s->account.id);
        draw_page("The Auction House", "Carried: " + gold_text(py.misc.au), "Waiting for you: " + std::to_string(claims), rows, top,
                  {"b) Bid", "o) Buy out", "s) Sell an item", "c) Collect", "+/-) Page", "ESC) Leave"});
        putString("(* = your bid is the highest)", Coord_t{18, 0});

        char cmd;
        if (!getCommand("", cmd)) {
            return;
        }
        if (cmd == 'b' || cmd == 'o') {
            int i = pick_row(cmd == 'b' ? "Bid on which lot?" : "Buy out which lot?", top, (int) all.size());
            if (i >= 0) {
                auction_bid(s, all[(size_t) i], cmd == 'o');
            }
        } else if (cmd == 's') {
            auction_sell(s);
        } else if (cmd == 'c') {
            collect_claims(s);
        } else {
            page_keys(cmd, top, (int) all.size());
        }
    }
}

void muEnterBuilding(int sub_category_id) {
    MuSession *s = mu_self;
    MuSafePrompt safe;
    switch (sub_category_id) {
        case 107:
            bank_screen(s);
            break;
        case 108:
            guild_screen(s);
            break;
        case 109:
            auction_screen(s);
            break;
        default:
            return;
    }
    drawCavePanel();
}

// --- the town -----------------------------------------------------------------

// Bank, Guild Hall and Auction House stand in the town's free margins, left
// and right of Umoria's six stores (dungeonBuildStore never uses x < 10 or
// x > 54). Doors are store doors with sub-categories 107-109.
void muPlaceTownBuildings() {
    static const struct {
        int top, bottom, left, right, door_y, door_x;
        char sprite;
    } b[3] = {
        {3, 7, 2, 7, 5, 7, '7'},       // Bank, door on its east wall
        {13, 17, 2, 7, 15, 7, '8'},    // Guild Hall
        {8, 12, 57, 63, 10, 57, '9'},  // Auction House, door on its west wall
    };
    for (int i = 0; i < 3; i++) {
        for (int y = b[i].top; y <= b[i].bottom; y++) {
            for (int x = b[i].left; x <= b[i].right; x++) {
                dg.floor[y][x].feature_id = TILE_BOUNDARY_WALL;
            }
        }
        Tile_t &door = dg.floor[b[i].door_y][b[i].door_x];
        door.feature_id = TILE_CORR_FLOOR;
        int id = popt();
        door.treasure_id = (uint8_t) id;
        inventoryItemCopyTo(config::dungeon::objects::OBJ_STORE_DOOR, game.treasure.list[id]);
        game.treasure.list[id].sub_category_id = (uint8_t) (107 + i);
        game.treasure.list[id].sprite = (uint8_t) b[i].sprite;
    }
}

static int64_t store_owner[MAX_STORES] = {0};

bool muStoreEnter(int store_id) {
    if (store_owner[store_id] != 0 && store_owner[store_id] != mu_self->account.id) {
        printMessage("The shopkeeper is busy with another customer.");
        return false;
    }
    store_owner[store_id] = mu_self->account.id;
    return true;
}

void muStoreLeave(int store_id) {
    if (store_owner[store_id] == mu_self->account.id) {
        store_owner[store_id] = 0;
    }
}

// --- trading ----------------------------------------------------------------

static void trade_release(MuSession *s) {
    MuTrade *tr = s->trade;
    if (tr == nullptr) {
        return;
    }
    s->trade = nullptr;
    if (--tr->refs == 0) {
        delete tr;
    }
}

void mu_trade_request(MuSession *s, const std::string &name) {
    pthread_mutex_lock(&mu_sessions_lock);
    MuSession *t = mu_find_online(name.c_str());
    pthread_mutex_unlock(&mu_sessions_lock);
    if (t == nullptr || !t->in_game || t == s) {
        mu_system(s, "Nobody by that name is in the game.");
        return;
    }
    if (t->level != s->level) {
        mu_system(s, "You must be on the same level to trade.");
        return;
    }
    if (s->trade != nullptr || t->trade != nullptr) {
        mu_system(s, "One of you is already trading.");
        return;
    }
    if (t->trade_asked != s->account.id) {
        s->trade_asked = t->account.id;
        mu_system(t, s->char_name + " wants to trade. Type /trade " + s->char_name + " to accept.");
        mu_system(s, "You ask " + t->char_name + " to trade.");
        return;
    }
    t->trade_asked = 0;
    auto *tr = new MuTrade();
    tr->side[0] = t;
    tr->side[1] = s;
    s->trade = t->trade = tr;
    t->event_pending = true;
    mu_conn_poke(t->conn);
    mu_trade_screen(s);
}

void mu_trade_cancel(MuSession *s) {
    MuTrade *tr = s->trade;
    if (tr == nullptr) {
        return;
    }
    if (tr->state == 0) {
        tr->state = 2;
        tr->note = s->char_name + " has gone.";
        MuSession *other = tr->side[0] == s ? tr->side[1] : tr->side[0];
        mu_conn_poke(other->conn);
    }
    trade_release(s);
}

static Player_t &player_of(MuSession *p) {
    return p == mu_self ? py : p->ctx.py;
}

// Both confirmed: swap, and save both characters in one transaction.
static bool trade_execute(MuSession *s, MuTrade *tr) {
    int me = tr->side[0] == s ? 0 : 1, them = 1 - me;
    MuSession *other = tr->side[them];
    Player_t &op = other->ctx.py; // the other player is not loaded

    // check: slots still there, gold carried, room (counting every incoming
    // item as a new slot, ignoring stacking - never too little room)
    for (int side = 0; side < 2; side++) {
        Player_t const &p = player_of(tr->side[side]);
        std::vector<int> seen;
        for (int slot : tr->slots[side]) {
            if (slot >= p.pack.unique_items || std::count(seen.begin(), seen.end(), slot) != 0) {
                tr->note = "An offered item is gone.";
                return false;
            }
            seen.push_back(slot);
        }
        if (tr->gold[side] > p.misc.au) {
            tr->note = tr->side[side]->char_name + " lacks the gold offered.";
            return false;
        }
        int after = p.pack.unique_items - (int) tr->slots[side].size() + (int) tr->slots[1 - side].size();
        if (after > PlayerEquipment::Wield) {
            tr->note = tr->side[side]->char_name + " has no room for it all.";
            return false;
        }
    }

    std::vector<Inventory_t> give[2];
    for (int side = 0; side < 2; side++) {
        Player_t const &p = player_of(tr->side[side]);
        for (int slot : tr->slots[side]) {
            give[side].push_back(p.inventory[slot]);
        }
    }
    auto swap_side = [&](int side) {
        std::vector<int> slots = tr->slots[side];
        std::sort(slots.rbegin(), slots.rend());
        for (int slot : slots) {
            take_slot(slot);
        }
        py.misc.au -= (int32_t) tr->gold[side];
        for (Inventory_t item : give[1 - side]) {
            inventoryCarryItem(item);
        }
        py.misc.au += (int32_t) tr->gold[1 - side];
    };

    Player_t before_me = py, before_them = op;
    if (mu_db_begin() != 0) {
        tr->note = "Try again in a moment.";
        return false;
    }
    swap_side(me);
    bool ok = mu_char_store(s);
    mu_with_player(s, other, [&] {
        swap_side(them);
        ok = ok && mu_char_store(other);
    });
    if (ok) {
        ok = mu_db_commit() == 0;
    } else {
        mu_db_rollback();
    }
    if (!ok) {
        py = before_me;
        other->ctx.py = before_them;
        tr->note = "The trade failed; nothing changed.";
        return false;
    }
    for (int side = 0; side < 2; side++) {
        std::string what;
        for (auto const &item : give[side]) {
            what += describe(item) + "; ";
        }
        mu_db_ledger(tr->side[side]->account.id, tr->side[side]->char_name.c_str(), "trade-give", -tr->gold[side], what.c_str(),
                     tr->side[1 - side]->char_name.c_str());
    }
    return true;
}

static void draw_trade(MuTrade *tr, int me) {
    int them = 1 - me;
    MuSession *other = tr->side[them];
    clearScreen();
    putString(("Trading with " + other->char_name).c_str(), Coord_t{2, 9});
    putString(tr->confirmed[me] ? "You: CONFIRMED" : "You:", Coord_t{4, 0});
    putString((other->char_name + (tr->confirmed[them] ? ": CONFIRMED" : ":")).c_str(), Coord_t{4, 40});
    for (int side = 0; side < 2; side++) {
        int col = side == me ? 0 : 40;
        Player_t const &p = player_of(tr->side[side]);
        int row = 5;
        for (size_t i = 0; i < tr->slots[side].size(); i++) {
            int slot = tr->slots[side][i];
            std::string text = slot < p.pack.unique_items ? describe(p.inventory[slot]) : "(gone)";
            putString((std::string(1, (char) ('a' + i)) + ") " + text.substr(0, 34)).c_str(), Coord_t{row++, col});
        }
        putString(gold_text(tr->gold[side]).c_str(), Coord_t{row + 1, col + 3});
    }
    putString("a) Offer an item", Coord_t{19, 0});
    putString("r) Take back an item", Coord_t{19, 40});
    putString("g) Offer gold", Coord_t{20, 0});
    putString("c) Confirm", Coord_t{20, 40});
    putString("ESC) Call it off", Coord_t{21, 0});
    putString("Any change clears both confirmations.", Coord_t{22, 0});
}

void mu_trade_screen(MuSession *s) {
    MuTrade *tr = s->trade;
    if (tr == nullptr) {
        return;
    }
    int me = tr->side[0] == s ? 0 : 1;
    MuSession *other = tr->side[1 - me];
    MuSafePrompt safe;
    s->wake_on_poke = true;

    auto changed = [&] {
        tr->confirmed[0] = tr->confirmed[1] = false;
        mu_conn_poke(other->conn);
    };

    while (tr->state == 0) {
        draw_trade(tr, me);
        if (!tr->note.empty()) {
            putStringClearToEOL(tr->note.c_str(), Coord_t{0, 0});
            tr->note.clear();
        }
        char cmd = getKeyInput();
        if (tr->state != 0) {
            break;
        }
        int slot = 0;
        int64_t gold = 0;
        switch (cmd) {
            case ESCAPE:
                tr->state = 2;
                tr->note = s->char_name + " called the trade off.";
                mu_conn_poke(other->conn);
                break;
            case 'a':
                if ((int) tr->slots[me].size() >= MU_TRADE_SLOTS) {
                    printMessage("That is as much as one trade holds.");
                } else if (pick_pack_item("Offer which item?", slot)) {
                    if (std::count(tr->slots[me].begin(), tr->slots[me].end(), slot) == 0) {
                        tr->slots[me].push_back(slot);
                        changed();
                    }
                }
                break;
            case 'r':
                if (!tr->slots[me].empty()) {
                    char which;
                    if (getCommand("Take back which (a, b, ...)?", which) && which >= 'a' && which < 'a' + (int) tr->slots[me].size()) {
                        tr->slots[me].erase(tr->slots[me].begin() + (which - 'a'));
                        changed();
                    }
                }
                break;
            case 'g':
                if (ask_number("Offer how much gold?", gold)) {
                    if (gold > py.misc.au) {
                        printMessage("You do not have that much.");
                    } else {
                        tr->gold[me] = gold;
                        changed();
                    }
                }
                break;
            case 'c':
                tr->confirmed[me] = true;
                if (tr->confirmed[1 - me]) {
                    if (trade_execute(s, tr)) {
                        tr->state = 1;
                        tr->note = "The trade is done.";
                    } else {
                        tr->confirmed[0] = tr->confirmed[1] = false;
                    }
                }
                mu_conn_poke(other->conn);
                break;
            default:
                break; // MU_KEY_EVENT: the other side changed something; redraw
        }
    }

    s->wake_on_poke = false;
    std::string note = tr->note;
    trade_release(s);
    drawCavePanel();
    if (!note.empty()) {
        printMessage(note.c_str());
    }
}
