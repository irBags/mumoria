-- The Old Crypt (levels 2-5): a raid for new characters.
--
-- Its gate (0) stands at 100 ft. A crypt drawn by this script: rows of
-- tombs, an ossuary of bones, a niche behind a secret door (search!), and
-- the Crypt Keeper's candle-lit hall behind a locked door. The dead wake as
-- he weakens. Its decor (tombs, bones, the altar, candles) blocks the way and
-- answers when walked into.

mu.item{
  key = "gravecloth",
  name = "Gravecloth",
  base = "Cloak",
  tier = "uncommon",
  to_ac = 4,
  powers = {"resist_cold", "see_invisible"},
  from = "never",
}

local MAP = [[
################################################################
################################################################
##.........#####..........................######.............###
##.........#####..T..T..T..T..T..T..T..T..######..B.......B..###
##.<..@....+,,,,..........................######......z......###
##.........#####....s.......z.......s.....+,,^,,..B.s.....B..###
##.........#####........b.................######......G......###
######,#########..T..T..T..T..T..T..T..T..######..B.......B..###
######,#########..........................######.............###
######,###############################################+#########
######,###############################################,#########
######,###############################################,#########
######,###.........###################################,#########
######,###....H....###################################,#########
######,###.........#################.........................###
######,###.........#################............A............###
######,#######$#####################...C.................C...###
######,#######,#####################.........................###
######,,,,,,,,b,,,,,,,,,,,z,,,,,,,,&............L............###
####################################........s.......z........###
####################################...C.................C...###
####################################............G............###
####################################.........................###
################################################################
################################################################
################################################################
]]

local LINES = {
  T = {"The tomb's lid is carved with a name worn smooth.", "Something scratches inside this tomb.", "A cold draught seeps from the tomb."},
  B = {"Bones, stacked to the ceiling. Some of them are still moving.", "A skull grins at you from the pile."},
  A = {"An altar to something old. Dried blood cakes its edge."},
  C = {"The candle burns with a cold, blue flame."},
}

mu.raid{
  key = "old_crypt",
  name = "the Old Crypt",
  label = "Old Crypt",
  level = {2, 5},
  about = "The town's old crypt has woken. Its Keeper walks again, and he is not alone.",
  gate = 2,
  colour = 250,
  floors = {
    {depth = 2, map = MAP,
     legend = {
       T = {decor = "=", name = "a tomb", colour = 250},
       B = {decor = "&", name = "bones", colour = 230},
       A = {decor = "_", name = "the altar", colour = 124},
       C = {decor = "*", name = "a candle", colour = 75},
       s = {spawn = {creature = "Skeleton Kobold"}},
       z = {spawn = {creature = "Zombie Kobold", hp = 0.7}},
       b = {spawn = {creature = "Huge Brown Bat"}},
       G = {spawn = {creature = "Poltergeist"}},
       H = {item = "rare"},
     },
     mobs = {"Skeleton Kobold", "Poltergeist", "Huge Brown Bat"}, packs = 2, pack = {1, 2}},
  },
  lord = {creature = "Skeleton Kobold", name = "Crypt Keeper", hp = 4, at = "L",
          guard = {"Skeleton Kobold", "Zombie Kobold", "Poltergeist"}},
  loot = {tier = "uncommon", count = 3},
  reward = {tier = "uncommon", gold = 80, items = {{key = "gravecloth", name = "a Gravecloth cloak", one_in = 4}}},
  bump = function(p, d, letter)
    local lines = LINES[letter]
    if not lines then return false end
    mu.tell(p, "[Crypt] " .. lines[mu.random(#lines)])
  end,
}
