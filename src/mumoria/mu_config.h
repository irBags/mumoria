// Mumoria - server settings that are not in mumoria.conf.

#pragma once

#define MU_DEFAULT_PORT 4000
#define MU_MAX_CHARACTERS 5

#define MU_AUTOSAVE_SECONDS 120
#define MU_IDLE_SECONDS (30 * 60)
#define MU_BUSY_TIMEOUT_MS 5000 // turn-based groups: longest a stopped member holds the level
#define MU_BATTLE_RANGE 10      // squares: a member this near an awake monster is in the battle
#define MU_ROUND_TIMEOUT 20     // seconds before a battle round skips a member who has not acted
#define MU_NEW_MONSTER_MIN 40   // the most often a new monster can turn up: 1 turn in 40

#define MU_LEVEL_SAVE_SECONDS 60
#define MU_LEVEL_UNLOAD_SECONDS 600
#define MU_LEVEL_RESET_HOURS 6

#define MU_PARTY_MAX 5
#define MU_CHAT_MAX_LEN 200
#define MU_CHAT_BURST 5 // lines per 5 seconds

#define MU_BANK_SLOTS 50
#define MU_GUILD_VAULT_SLOTS 100
#define MU_AH_CUT_PERCENT 5
#define MU_AH_MAX_PER_SELLER 10
#define MU_TRADE_SLOTS 8

#define MU_STORE_RESTOCK_HOURS 4        // a store's shared supply of an item refills over this
#define MU_STORE_PERSONAL_HOURS 1       // a player's own stock comes back this long after they buy
#define MU_STORE_TURNOVER_SECONDS 600   // Umoria's stock turnover, at most this often

#define MU_BOSS_INTERVAL 2
#define MU_BOSS_RESPAWN_HOURS 2
#define MU_BOSS_TIER_CAP 10
