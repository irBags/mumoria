-- The Kobold Den (levels 1-3): the first raid, for new characters.
--
-- Its gate (0) stands at 50 ft. Inside, a cave the game makes fresh for each
-- party: kobolds and their pets in small sleeping packs, a Kobold Sergeant,
-- and the Kobold Chief deep within, who calls his guard as he weakens. Bring
-- a friend or two. The Dungeon Warden (50_dungeon_warden.lua) lists it too.

mu.raid{
  key = "kobold_den",
  name = "the Kobold Den",
  label = "Kobold Den",
  level = {1, 3},
  about = "Kobolds have dug a den under the town. A first raid: bring a friend.",
  gate = 1,
  floors = {
    {depth = 1, generate = true,
     mobs = {"Kobold", "Kobold", "Jackal", "Large White Snake", "Giant Yellow Centipede", "Giant White Centipede"},
     packs = 6, pack = {2, 3},
     captains = 1, captain = {creature = "Kobold", name = "Kobold Sergeant", hp = 3}},
  },
  lord = {creature = "Kobold", name = "Kobold Chief", hp = 5, guard = {"Kobold", "Jackal"}},
  loot = {tier = "uncommon", count = 2},
  reward = {tier = "uncommon", gold = 60},
}
