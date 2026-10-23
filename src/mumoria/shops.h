// Mumoria - the town's stores, shared by everyone. See shops.cpp.

#pragma once

#include "../headers.h"

struct MuSession;

// The stores as everyone shares them. stores[] holds the same, except the
// store the loaded player is in, which holds their own view of it.
extern Store_t mu_catalog[MAX_STORES];
extern int mu_store_supply_percent; // mumoria.conf store_supply_percent

// What a player's dealings with one store's owner have come to (Umoria keeps
// these in Store_t): kept for each player, for the session.
struct MuStoreTerms {
    bool set;
    int32_t turns_left_before_closing;
    int16_t insults_counter;
    uint16_t good_purchases, bad_purchases;
};

void mu_shops_take_catalog();           // stores[] has just been loaded or made: it is the catalog
void mu_shops_put_away(MuSession *s);    // save_player: s's view out of stores[]
void mu_shops_bring_back(MuSession *s);  // load_player: s's view back in
void mu_set_inside(MuSession *s, bool inside); // screens.cpp
