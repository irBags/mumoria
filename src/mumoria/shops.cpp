// Mumoria - the town's six stores, shared by everyone. See
// docs/MUMORIA_DESIGN.md 2.19.
//
// The stock everyone shares (the catalog) is what Umoria's stores hold. Each
// player shops in their own copy of a store, made from the catalog when they
// go in and swapped in and out with the rest of their globals, so any number
// can shop at once:
//   * their own stock: of each line they can buy what Umoria's store has of
//     it (more of the goods sold one at a time: allowance()), and it comes
//     back MU_STORE_PERSONAL_HOURS after their first buy;
//   * the shared supply: every purchase, by anyone, also comes out of the
//     store's supply of that item, which refills over MU_STORE_RESTOCK_HOURS.
//     When it is gone, it is gone for everyone until it refills.

#include "shops.h"

#include "mu_config.h"
#include "session.h"
#include "social.h"

Store_t mu_catalog[MAX_STORES];
int mu_store_supply_percent = 100;

namespace {

time_t last_turnover = 0;

// One line of a store: the item, whatever its count.
std::string key_of(Inventory_t const &item) {
    char key[96];
    snprintf(key, sizeof(key), "%u:%u:%u:%d:%d:%d:%d", item.category_id, item.sub_category_id, item.special_name_id, item.misc_use, item.to_hit,
             item.to_damage, item.to_ac);
    return key;
}

// The shared stock's line for an item, or null (something a player sold here).
Inventory_t const *in_catalog(int store_id, std::string const &key) {
    Store_t const &c = mu_catalog[store_id];
    for (int i = 0; i < c.unique_items_counter; i++) {
        if (key_of(c.inventory[i].item) == key) {
            return &c.inventory[i].item;
        }
    }
    return nullptr;
}

// How much of an item the whole server can buy before it refills: plenty of
// the cheap things everyone needs, a few of anything worth having.
int supply_cap(Inventory_t const &item) {
    int32_t value = storeItemValue(item);
    int cap = value <= 25 ? 300 : value <= 100 ? 120 : value <= 500 ? 40 : value <= 2000 ? 12 : 4;
    cap = cap * mu_store_supply_percent / 100;
    return std::max(cap, std::max(1, (int) item.items_count)); // a full store's stack, at least
}

// How many of a line one player can buy before their own stock comes back:
// for goods sold one at a time (food, oil, potions) more than Umoria's store
// happens to hold; for anything else, what it holds (a cloak is one cloak).
int allowance(Inventory_t const &item) {
    int count = std::max(1, (int) item.items_count);
    if (!inventoryItemSingleStackable(item)) {
        return count;
    }
    int32_t value = storeItemValue(item);
    return std::max(count, value <= 25 ? 10 : value <= 100 ? 5 : value <= 500 ? 3 : 1);
}

// The supply left of an item, refilled for the time since it was last
// counted (a partial unit's time is kept).
int supply_left(int store_id, std::string const &key, int cap) {
    int64_t now = (int64_t) time(nullptr);
    int64_t period = (int64_t) MU_STORE_RESTOCK_HOURS * 3600;
    MuRows *r = mu_db_query("SELECT remaining, updated_at FROM store_supply WHERE store = ? AND item = ?", "is", (int64_t) store_id, key.c_str());
    bool found = mu_db_next(r) != 0;
    int64_t remaining = found ? mu_db_col_int(r, 0) : cap;
    int64_t updated = found ? mu_db_col_int(r, 1) : now;
    mu_db_done(r);

    if (remaining >= cap) {
        remaining = cap;
        updated = now; // full: the refill clock starts when some is bought
    } else {
        int64_t gained = (now - updated) * cap / period;
        if (gained > 0) {
            remaining += gained;
            updated += gained * period / cap;
            if (remaining >= cap) {
                remaining = cap;
                updated = now;
            }
        }
    }
    mu_db_run("INSERT INTO store_supply (store, item, remaining, cap, updated_at) VALUES (?, ?, ?, ?, ?) "
              "ON CONFLICT(store, item) DO UPDATE SET remaining = excluded.remaining, cap = excluded.cap, updated_at = excluded.updated_at",
              "isiii", (int64_t) store_id, key.c_str(), remaining, (int64_t) cap, updated);
    return (int) remaining;
}

// What this character has bought of an item since their own stock last came back.
int bought(MuSession *s, int store_id, std::string const &key) {
    int64_t now = (int64_t) time(nullptr);
    MuRows *r = mu_db_query("SELECT count, since FROM store_bought WHERE character = ? AND store = ? AND item = ?", "iis", s->char_id, (int64_t) store_id,
                            key.c_str());
    int count = 0;
    if (mu_db_next(r) && now - mu_db_col_int(r, 1) < (int64_t) MU_STORE_PERSONAL_HOURS * 3600) {
        count = (int) mu_db_col_int(r, 0);
    }
    mu_db_done(r);
    return count;
}

void remove_line(Store_t &store, int i) {
    for (int j = i; j < store.unique_items_counter - 1; j++) {
        store.inventory[j] = store.inventory[j + 1];
    }
    store.unique_items_counter--;
}

} // namespace

void mu_shops_take_catalog() {
    memcpy(mu_catalog, stores, sizeof(mu_catalog));
}

void mu_shops_put_away(MuSession *s) {
    if (s->store_in >= 0) {
        s->store_view = stores[s->store_in];
        stores[s->store_in] = mu_catalog[s->store_in];
    }
}

void mu_shops_bring_back(MuSession *s) {
    if (s->store_in >= 0) {
        stores[s->store_in] = s->store_view;
    }
}

bool muStoreEnter(int store_id) {
    MuSession *s = mu_self;
    MuStoreTerms &terms = s->store_terms[store_id];
    if (!terms.set) {
        Store_t const &c = mu_catalog[store_id];
        terms = MuStoreTerms{true, c.turns_left_before_closing, c.insults_counter, c.good_purchases, c.bad_purchases};
    }
    if (terms.turns_left_before_closing >= dg.game_turn) {
        printMessage("The doors are locked.");
        return false;
    }

    Store_t view = mu_catalog[store_id];
    view.turns_left_before_closing = terms.turns_left_before_closing;
    view.insults_counter = terms.insults_counter;
    view.good_purchases = terms.good_purchases;
    view.bad_purchases = terms.bad_purchases;
    for (int i = view.unique_items_counter - 1; i >= 0; i--) {
        Inventory_t &item = view.inventory[i].item;
        std::string key = key_of(item);
        int mine = allowance(item) - bought(s, store_id, key);
        int shared = supply_left(store_id, key, supply_cap(item));
        int n = std::min(mine, shared);
        if (n <= 0) {
            remove_line(view, i);
        } else {
            item.items_count = (uint8_t) n;
        }
    }
    stores[store_id] = view;
    s->store_in = store_id;
    mu_set_inside(s, true);
    return true;
}

void muStoreLeave(int store_id) {
    MuSession *s = mu_self;
    if (s->store_in != store_id) {
        return;
    }
    Store_t const &view = stores[store_id];
    s->store_terms[store_id] = MuStoreTerms{true, view.turns_left_before_closing, view.insults_counter, view.good_purchases, view.bad_purchases};
    stores[store_id] = mu_catalog[store_id];
    s->store_in = -1;
    mu_set_inside(s, false);
}

bool muStoreCanBuy(int store_id, Inventory_t const &item) {
    std::string key = key_of(item);
    Inventory_t const *line = in_catalog(store_id, key);
    if (line == nullptr) {
        return true; // something this player sold here: theirs to buy back
    }
    if (supply_left(store_id, key, supply_cap(*line)) < std::max(1, (int) item.items_count)) {
        printMessage("Sorry, that has just sold out. More is on the way.");
        return false;
    }
    return true;
}

void muStoreBought(int store_id, Inventory_t const &item) {
    std::string key = key_of(item);
    Inventory_t const *line = in_catalog(store_id, key);
    if (line == nullptr) {
        return;
    }
    int n = std::max(1, (int) item.items_count);
    int64_t now = (int64_t) time(nullptr);
    int left = supply_left(store_id, key, supply_cap(*line)); // also starts the refill clock if it was full
    mu_db_run("UPDATE store_supply SET remaining = ? WHERE store = ? AND item = ?", "iis", (int64_t) std::max(0, left - n), (int64_t) store_id,
              key.c_str());

    int had = bought(mu_self, store_id, key);
    mu_db_run("INSERT INTO store_bought (character, store, item, count, since) VALUES (?, ?, ?, ?, ?) "
              "ON CONFLICT(character, store, item) DO UPDATE SET count = excluded.count, since = CASE WHEN ? = 0 THEN excluded.since ELSE since END",
              "iisiii", mu_self->char_id, (int64_t) store_id, key.c_str(), (int64_t) (had + n), now, (int64_t) had);
}

void muStoreTurnover() {
    time_t now = time(nullptr);
    if (now - last_turnover < MU_STORE_TURNOVER_SECONDS) {
        return;
    }
    last_turnover = now;
    // the loaded player is in the dungeon, so stores[] holds the catalog
    storeMaintenance();
    mu_shops_take_catalog();
}
