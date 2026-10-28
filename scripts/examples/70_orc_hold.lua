-- The Orc Stronghold (levels 8-14): a raid three floors deep.
--
-- Its gate (0) stands at 350 ft. The Outer Caves and the Barracks are caves
-- the game makes fresh for each party, each with stairs down (>) as far
-- from the way in as can be found; the War Chief's Hall below is drawn here.
-- The floors a party has opened stay open while any of them is inside, so
-- the stairs up (<) lead back the way they came. Kill the War Chief to win.

mu.item{
  key = "war_chiefs_cleaver",
  name = "War Chief's Cleaver",
  base = "Beaked Axe",
  tier = "rare",
  to_hit = 6, to_dam = 8,
  powers = {"slay_animal", "str"},
  bonus = 1,
  from = "never",
}

local HALL = [[
######################################################################
######################################################################
######################################################################
######################################################################
################......W.....W.....W.....W.....W.....##################
################..F..............................F..##################
################....................................##################
################....#.....#.....#.....#.....#.......####...........###
################................o...................####...........###
##.......#######........o...........................####....g......###
##.......#######........................s...........####...........###
##.<.@...,,,,,,+....................................',,,.......L.T.###
##.......#######....................................####...........###
##.......#######........o...........................####....g......###
################................o...................####...........###
################....#.....#.....#.....#.....#.......####...........###
################....................................##################
################..F..............................F..##################
################....................................##################
##########################+###########################################
####################............######################################
####################..X..H...X..######################################
####################............######################################
######################################################################
]]

mu.raid{
  key = "orc_hold",
  name = "the Orc Stronghold",
  label = "Orc Stronghold",
  level = {8, 14},
  about = "Three floors of orcs, dug into the rock. Their War Chief holds the deepest hall.",
  gate = 7,
  colour = 166,
  floors = {
    {name = "the Outer Caves", label = "Orc Caves", depth = 7, generate = true,
     mobs = {"Orc", "Orc", "Orc Shaman", "Jackal", "Skeleton Orc"},
     packs = 8, pack = {3, 5},
     captains = 1, captain = {creature = "Orc Warrior", name = "Orc Sergeant", hp = 3}},
    {name = "the Barracks", label = "Orc Barracks", depth = 9, generate = true,
     mobs = {"Orc Warrior", "Orc Warrior", "Orc Shaman", "Orc", "Hobgoblin", "Skeleton Orc"},
     packs = 9, pack = {3, 5},
     captains = 2, captain = {creature = "Black Orc", name = "Orc Captain", hp = 3}},
    {name = "the War Chief's Hall", label = "War Chief's Hall", depth = 11, map = HALL,
     legend = {
       F = {decor = "*", name = "a brazier", colour = 208},
       W = {decor = "|", name = "a banner", colour = 160},
       T = {decor = "=", name = "the throne", colour = 136},
       X = {decor = "&", name = "a barrel", colour = 94},
       o = {spawn = {creature = "Orc Warrior"}},
       s = {spawn = {creature = "Orc Shaman"}},
       g = {spawn = {creature = "Black Orc"}},
       H = {item = "rare"},
     },
     mobs = {"Orc Warrior", "Black Orc", "Orc Shaman"}, packs = 2, pack = {2, 3}},
  },
  lord = {creature = "Black Orc", name = "Orc War Chief", hp = 6, speed = 1, at = "L",
          guard = {"Orc Warrior", "Black Orc", "Orc Shaman"}},
  loot = {tier = "rare", count = 3},
  reward = {tier = "rare", gold = 400, items = {{key = "war_chiefs_cleaver", name = "the War Chief's Cleaver", one_in = 4}}},
  enter = function(p, d)
    if d.floor == 3 and not d.flags.drums then
      d.flags.drums = true
      mu.say_level("[Raid] War drums pound in the hall ahead. The War Chief knows you are here.")
    end
  end,
  bump = function(p, d, letter)
    if letter == "T" then mu.tell(p, "[Raid] A throne of bones and iron. Someone has carved 'MINE' into the arm.") end
    if letter == "X" then mu.tell(p, "[Raid] The barrel reeks of orc grog.") end
    return letter == "T" or letter == "X"
  end,
}
