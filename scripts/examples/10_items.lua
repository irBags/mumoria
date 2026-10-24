-- Example custom items. Copy this file into scripts/ to use it.
--
-- Each item is a base item with its own name, rarity, bonuses and powers.
-- `from` is where it may drop (anywhere, bosses, lords, never) and `one_in`
-- how often: 1 in that many gear drops there, between the two depths.
-- Change a line and /reload: items already found keep what they had.

mu.item{
  key = "thunderfury",
  name = "Thunderfury",
  base = "Longsword",
  tier = "legendary",
  to_hit = 14, to_dam = 16,
  dice = "3d6",
  powers = {"slay_evil", "flame_tongue", "free_action", "dex"},
  bonus = 2,                      -- +2 Dexterity
  depth = {20, 127},
  from = "lords",
  one_in = 40,
}

mu.item{
  key = "wanderers_boots",
  name = "of the Wanderer",       -- a name starting "of " reads "Boots of the Wanderer"
  base = "Pair of Hard Leather Boots",
  tier = "epic",
  to_ac = 12,
  powers = {"speed", "feather_fall"},
  bonus = 1,                      -- +1 speed
  depth = {10, 60},
  from = "bosses",
  one_in = 60,
}

mu.item{
  key = "glimmering_band",
  name = "Glimmering Band",
  base = "Ring of Protection",
  tier = "rare",
  to_ac = 10,
  powers = {"see_invisible", "resist_cold"},
  depth = {5, 40},
  from = "anywhere",
  one_in = 2000,
}
