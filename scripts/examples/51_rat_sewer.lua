-- The Rat-King's Sewer (levels 5-10). Needs 50_dungeon_warden.lua to get in.
--
-- Dark tunnels under the town, full of vermin. A sluice gate (G) bars the
-- Rat King's den; two rusted levers (L, M) in rooms east and west raise it.
-- The King squeals for his brood as he weakens. His hoard (H) spills when he
-- dies, and each of you has a reward once every 12 hours.

local KEY = "rat_sewer"
local TITLE = "Sewer"

mu.item{
  key = "rat_kings_crown",
  name = "Rat King's Crown",
  base = "Silver Crown",
  tier = "rare",
  to_ac = 6,
  powers = {"see_invisible", "stealth"},
  bonus = 2,
  from = "never",
}

local MAP = [[
############################################################################################
############################################################################################
##...........##################,,,,,,,,,,,,,,,b#############............s###################
##...........##################,,r,,,,,,,,,,,,,#############.c...........###################
##.<..@......+,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,#############.............###################
##...........##################,,,,,f,,,,,,,,,,,,,,,^,,,,,,+......M......###################
##...........##################,,,,,,,,,r,,,,,,#############.............###################
###############################,,,,,,,,,,,f,,,,#############...........c.###################
###############################,,,,,,,,,,,,,r,,#############.............###################
###############################,,,r,,,,,,,,,,,,#############################################
###############################,,,,,,,,,,,,,,,,#############################################
######################################,#####################################################
######################################,#####################################################
######################################,#####################################################
######################################,#################################..............HH..##
######################################i#################################..w.............H.##
###...........s#######################,#################################..................##
###.j..........#######################,#################################......w...........##
###............#######################,#################################...............w..##
###.....L......#######################,#######################,,j,,,,,,G..................##
###............+,,,,,,,,,,^,,,,,,,,,,,,,,,,,,,c,,,,,,r,,,,,,,,,,,,,,,,,G...........K......##
###............###############################################,,,,,,f,,G..................##
###..........j.#########################################################..................##
###............#########################################################.......w..........##
########################################################################..................##
########################################################################..............w...##
########################################################################..w...............##
########################################################################..................##
############################################################################################
############################################################################################
]]

local function rats(near, n)
  mu.spawn{creature = "Giant White Rat", near = near, count = n, awake = true}
end

local KING_PHASES = {
  {at = 66, run = function(m)
    mu.say_level("The Rat King squeals! Rats pour out of the drains!")
    rats(m, 4)
  end},
  {at = 33, run = function(m)
    mu.say_level("The Rat King gnaws a fat grub and swells with new strength!")
    mu.heal(m, m.max_hp // 4)
    rats(m, 5)
  end},
}

local function pull(p, d, which)
  if d.flags[which] then return end
  d.flags[which] = true
  mu.say_level("[Sewer] " .. p.name .. " heaves a rusted lever. Somewhere a sluice gate groans.")
  if d.flags.L and d.flags.M then
    for _, at in ipairs(d.places.G) do mu.tile(at, ",") end
    mu.say_level("[Sewer] With a roar of foul water, the sluice gate rises. Something big chitters beyond it.")
  end
end

mu.dungeon{
  key = KEY,
  name = "the Rat-King's Sewer",
  label = "Rat-King's Sewer",
  depth = 6,
  level = {5, 10},
  about = "vermin in the dark under the town, two levers, and the Rat King.",
  map = MAP,
  legend = {
    G = "#",
    r = {spawn = {creature = "Giant White Rat"}},
    w = {spawn = {creature = "Giant Grey Rat"}},
    f = {spawn = {creature = "Giant Black Frog"}},
    b = {spawn = {creature = "Huge Brown Bat"}},
    c = {spawn = {creature = "Giant Black Centipede"}},
    s = {spawn = {creature = "Large Grey Snake"}},
    j = {spawn = {creature = "Rot Jelly"}},
    i = {spawn = {creature = "Grey Icky-Thing"}},
  },
  build = function(d)
    local k = d.places.K[1]
    mu.spawn{creature = "Giant Grey Rat", name = "Rat King", at = k, boss = "rat_king",
             hp = 40 * (1 + (d.party - 1) / 2), speed = 1}
  end,
  enter = function(p, d)
    mu.tell(p, "[Sewer] The stench is unbelievable. Somewhere ahead, a sluice gate holds back... something.")
  end,
  step = function(p, d, letter)
    if letter == "L" or letter == "M" then pull(p, d, letter) end
  end,
  hurt = function(p, m, d)
    if m.boss == "rat_king" then mu.phases(m, KING_PHASES) end
  end,
  kill = function(p, m, d)
    if m.boss ~= "rat_king" then return end
    for _, at in ipairs(d.places.H) do mu.drop("uncommon", at) end
    mu.drop("rare", d.places.H[1])
    mu.say_level("[Sewer] The Rat King is dead! His hoard lies in the corner of the den.")
    mu.say_world(p.name .. (d.party > 1 and "'s party has" or " has") .. " slain the Rat King!")
    mu.reward_party{key = KEY, title = TITLE, tier = "rare", gold = 250,
                    items = {{key = "rat_kings_crown", name = "the Rat King's Crown", one_in = 4}}}
  end,
}
