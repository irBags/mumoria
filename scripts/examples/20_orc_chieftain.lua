-- Example boss fight. Copy this file into scripts/ to use it.
--
-- On 300 ft (level 6) a Black Orc chieftain waits with his warband. He
-- calls more orcs at 70%, his shamans at 35% (and heals), drops an Epic
-- cleaver now and then plus two Epic pieces, and comes back 6 hours after
-- he dies.

mu.item{
  key = "chieftains_cleaver",
  name = "Grashnak's Cleaver",
  base = "Broad Axe",
  tier = "epic",
  to_hit = 8, to_dam = 10,
  powers = {"slay_animal", "regeneration", "str"},
  bonus = 1,
  from = "never",                 -- only from Grashnak (below), never a random drop
}

mu.boss{
  key = "grashnak",
  depth = 6,
  creature = "Black Orc",
  hp = 10,                        -- ten times a Black Orc's hit points
  speed = 1,
  respawn_hours = 6,
  announce = "War drums echo through the halls: Grashnak the Chieftain holds this level.",
  guards = {creature = "Orc Warrior", count = 4},
  phases = {
    {at = 70, say = "Grashnak bellows, and his warband comes running!", spawn = {creature = "Orc Warrior", count = 3}},
    {at = 35, say = "Grashnak roars for his shamans, and his wounds close!", spawn = {creature = "Orc Shaman", count = 2}, heal = 60},
  },
  loot = {items = {{key = "chieftains_cleaver", one_in = 3}}, tier = "epic", count = 2},
  defeat = "{player} has slain Grashnak the Chieftain!",
}
