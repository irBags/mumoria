-- The Sunken Temple (levels 15-22): a raid two floors deep.
--
-- Its gate (0) stands at 600 ft. The Flooded Halls are a cave the game
-- makes fresh for each party, full of serpents; the stairs down (>) lead to
-- the Drowned Sanctum, drawn here: a nave of dark pools and statues, and
-- the Naga Queen at her altar. Halfway down she drinks from the pools.

mu.item{
  key = "naga_queens_spear",
  name = "Naga Queen's Spear",
  base = "Spear",
  tier = "epic",
  to_hit = 10, to_dam = 10,
  dice = "2d8",
  powers = {"slay_animal", "resist_acid", "free_action", "dex"},
  bonus = 2,
  from = "never",
}

local SANCTUM = [[
########################################################################
########################################################################
########################################################################
########################################################################
########################################################################
##################..............................########################
##################........S...S...S...S.........########################
##################..............................########################
##################....WW..................WW....####................####
##################....WW..................WW....####..W...W...W.....####
##################....WW......n...........WW....####................####
###.......########....WW..................WW....####......E.........####
###.......########..............................####................####
###.<..@..,,,,,,,,+.......R.........R...........',,,...........L..A.####
###.......########..............................####................####
###.......########....WW..................WW....####......E.........####
##################....WW......n...........WW....####................####
##################....WW..................WW....####..W...W...W.....####
##################....WW..................WW....####................####
##################..............................########################
##################........S...S...S...S.........########################
##################..............................########################
########################################################################
########################################################################
########################################################################
########################################################################
]]

local QUEEN = {
  {at = 75, run = function(m)
    mu.say_level("[Raid] The Naga Queen hisses, and serpents slither from the pools!")
    mu.spawn{creature = "King Cobra", near = m, count = 2, awake = true}
  end},
  {at = 50, run = function(m)
    mu.say_level("[Raid] The Naga Queen sinks into a pool and rises, whole again in part.")
    mu.heal(m, m.max_hp // 5)
    mu.spawn{creature = "Water Spirit", near = m, count = 2, awake = true}
  end},
  {at = 25, run = function(m)
    mu.say_level("[Raid] The Naga Queen shrieks! The water itself rises against you!")
    mu.spawn{creature = "Water Spirit", near = m, count = 3, awake = true}
  end},
}

mu.raid{
  key = "sunken",
  name = "the Sunken Temple",
  label = "Sunken Temple",
  level = {15, 22},
  about = "A temple the sea took. Its serpents still worship there, and their queen rules them.",
  gate = 12,
  colour = 33,
  floors = {
    {name = "the Flooded Halls", label = "Flooded Halls", depth = 14, generate = true,
     mobs = {"King Cobra", "Black Mamba", "Giant Red Speckled Frog", "Red Naga", "Grey Icky-Thing", "Water Spirit"},
     packs = 9, pack = {3, 5},
     captains = 2, captain = {creature = "Guardian Naga", name = "Temple Guardian", hp = 2}},
    {name = "the Drowned Sanctum", label = "Drowned Sanctum", depth = 17, map = SANCTUM,
     legend = {
       W = {decor = "~", name = "dark water", colour = 25},
       S = {decor = "&", name = "a statue", colour = 248},
       A = {decor = "_", name = "the altar", colour = 37},
       n = {spawn = {creature = "Red Naga"}},
       R = {spawn = {creature = "King Cobra"}},
       E = {spawn = {creature = "Water Spirit"}},
     },
     mobs = {"Black Mamba", "King Cobra"}, packs = 2, pack = {2, 3}},
  },
  lord = {creature = "Guardian Naga", name = "Naga Queen", hp = 6, speed = 1, at = "L", phases = QUEEN,
          guard = {"Black Mamba", "King Cobra"}},
  loot = {tier = "rare", count = 4},
  reward = {tier = "epic", gold = 900, items = {{key = "naga_queens_spear", name = "the Naga Queen's Spear", one_in = 5}}},
  bump = function(p, d, letter)
    if letter == "W" then mu.tell(p, "[Raid] The water is black and very deep. Something moves in it.") return true end
    if letter == "S" then mu.tell(p, "[Raid] A serpent-headed statue, its eyes two green stones.") return true end
    return false
  end,
}
