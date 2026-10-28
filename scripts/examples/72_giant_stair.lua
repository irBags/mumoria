-- The Giants' Stair (levels 18-28): a raid four floors deep.
--
-- Its gate (0) stands at 800 ft. Each floor down the stair belongs to a
-- different giant kin: the Hill Gate, the Frost Hall, the Forge (caves the
-- game makes fresh for each party), and at the bottom the Cloud Throne,
-- drawn here, where the Cloud Giant King sits among braziers. Each floor has
-- its own chieftain. A long way down: the floors stay open while the party
-- is inside.

mu.item{
  key = "cloud_kings_crown",
  name = "Cloud King's Crown",
  base = "Golden Crown",
  tier = "epic",
  to_ac = 12,
  powers = {"str", "con", "free_action", "feather_fall", "resist_lightning"},
  bonus = 3,
  from = "never",
}

local THRONE = [[
############################################################################
############################################################################
############################################################################
############################################################################
################################################........................####
################################################........................####
##############............................######..F.................F...####
##############............................######........................####
##############............................######........................####
##############...#....#....#....#....#....######........................####
##############............................######..F.................F...####
##############............................######............Q...........####
##############........P...................######........................####
##.......#####............................######........................####
##.......#####............................######..F.................F...####
##.<..@..,,,,,+.................P.........',,,,,..................L...T.####
##.......#####............................######........................####
##.......#####............................######........................####
##############........P...................######..F.................F...####
##############............................######............Q...........####
##############............................######........................####
##############...#....#....#....#....#....######........................####
##############............................######..F.................F...####
##############............................######........................####
##############............................######........................####
################################################........................####
################################################........................####
############################################################################
############################################################################
############################################################################
]]

local KING = {
  {at = 75, run = function(m)
    mu.say_level("[Raid] The Cloud Giant King stamps, and the hall shakes. His guard comes running!")
    mu.spawn{creature = "Stone Giant", near = m, count = 2, awake = true}
  end},
  {at = 50, run = function(m)
    mu.say_level("[Raid] Thunder rolls: the Cloud Giant King calls the storm to him!")
    mu.spawn{creature = "Air Spirit", near = m, count = 4, awake = true}
    mu.spawn{creature = "Frost Giant", near = m, count = 1, awake = true}
  end},
  {at = 20, run = function(m)
    mu.say_level("[Raid] The Cloud Giant King roars and fights on in a fury!")
    mu.heal(m, m.max_hp // 6)
    mu.spawn{creature = "Fire Giant", near = m, count = 2, awake = true}
  end},
}

mu.raid{
  key = "giant_stair",
  name = "the Giants' Stair",
  label = "Giants' Stair",
  level = {18, 28},
  about = "A stair of four halls, one for each giant kin. The Cloud Giant King rules the last.",
  gate = 16,
  colour = 252,
  floors = {
    {name = "the Hill Gate", label = "Hill Gate", depth = 16, generate = true,
     mobs = {"Hill Giant", "Ogre", "Ogre", "Black Orc", "Yeti"},
     packs = 7, pack = {2, 4},
     captains = 1, captain = {creature = "Hill Giant", name = "Hill Giant Chieftain", hp = 3}},
    {name = "the Frost Hall", label = "Frost Hall", depth = 18, generate = true,
     mobs = {"Frost Giant", "Yeti", "White Dragon Bat", "Giant White Dragon Fly"},
     packs = 7, pack = {2, 4},
     captains = 1, captain = {creature = "Frost Giant", name = "Frost Giant Jarl", hp = 3}},
    {name = "the Forge", label = "Giants' Forge", depth = 20, generate = true,
     mobs = {"Fire Giant", "Fire Spirit", "Hill Giant", "Giant Red Bat"},
     packs = 8, pack = {2, 4},
     captains = 2, captain = {creature = "Fire Giant", name = "Fire Giant Smith", hp = 3}},
    {name = "the Cloud Throne", label = "Cloud Throne", depth = 22, map = THRONE,
     legend = {
       F = {decor = "*", name = "a brazier", colour = 214},
       T = {decor = "=", name = "the throne", colour = 255},
       P = {spawn = {creature = "Stone Giant"}},
       Q = {spawn = {creature = "Fire Giant"}},
     },
     mobs = {"Stone Giant", "Frost Giant", "Fire Giant"}, packs = 2, pack = {1, 2}},
  },
  lord = {creature = "Cloud Giant", name = "Cloud Giant King", hp = 6, speed = 1, at = "L", phases = KING,
          guard = {"Stone Giant", "Frost Giant"}},
  loot = {tier = "epic", count = 3},
  reward = {tier = "epic", gold = 1500, items = {{key = "cloud_kings_crown", name = "the Cloud King's Crown", one_in = 5}}},
  bump = function(p, d, letter)
    if letter == "T" then
      mu.tell(p, "[Raid] A throne of white stone, three times your height. The cushion is a cloud.")
      return true
    end
    return false
  end,
}
