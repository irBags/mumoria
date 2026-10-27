-- The Bandit Fort (levels 10-15). Needs 50_dungeon_warden.lua to get in.
--
-- Open ground around a walled fort. Spellslingers watch from the corner
-- towers, bandits hold the yard and the barracks. In the jail a prisoner
-- waits behind bars (D) that only open when the Jailer (J) dies; walk into
-- him to talk once he is free: he pays, and tells where the chief hides his
-- strongbox (a secret door, S). The Bandit Chief (C) calls his guards and
-- drinks his potions as he weakens.

local KEY = "bandit_fort"
local TITLE = "Fort"

mu.item{
  key = "bandit_kings_cloak",
  name = "of the Bandit King",
  base = "Cloak",
  tier = "rare",
  to_ac = 8,
  powers = {"stealth", "dex"},
  bonus = 2,
  from = "never",
}

local MAP = [[
########################################################################################################################
#......................................................................................................................#
#......................................................................................................................#
#......................................................................................................................#
#......................................................................................................................#
#.........#............................................................................................................#
#..........#............................#######################################################################........#
#.......................................#######.........................................................#######........#
#.......................................##....#...................#########################.............#....##........#
#.......................................##.m..#...................#.......................#.............#.m..##........#
#...................b...................##....#...................#...b...........b.......#.............#....##........#
#.......................................###+###...................#.....................b.#.............##+####........#
#.......................................#.........................#........b..............#...................#........#
#.......................................#.........................#.g.................b...#...................#........#
#.......................................#.........b...............#.......................##############......#........#
#.............................b.........#.........................############+#############...........#......#........#
#.......................................#...............g..................................#...........#......#........#
#.......................................#..................................................#...g.....g.#......#........#
#.......................................#...................b..............................#...........########........#
#.......................................#..................................................#...........#..X..##........#
#...<.#.@...............................'..................................................+.......C...S...X.##........#
#.......................................#..................................................#...........#..X..##........#
#.......................................#.......................b..........................#...........########........#
#.......................................#..................................................#...g.....g.#......#........#
#.......................................#...........b.............############+#############...........#......#........#
#................................b......#...................g.....#.......................##...........#......#........#
#.......................................#.........................#.....J.................##############......#........#
#.......................................#.........................#.......................#...................#........#
#.....................b.................#.........................#.......................#...................#........#
#.......................................###+###...................##..&..###..D..###..&..##.............##+####........#
#.............#.........................##....#...................##..:..###..P..###.....##.............#....##........#
#.......................................##.m..#...................##.....###.....###...z.##.............#.m..##........#
#.......................................##....#...................#########################.............#....##........#
#.......................................#######.........................................................#######........#
#.......................................#######################################################################........#
#......................................................................................................................#
#.....................................................................#................................................#
#......................................................................................................................#
#......................................................................................................................#
########################################################################################################################
]]

mu.npc("prisoner")

local CHIEF_PHASES = {
  {at = 70, run = function(m)
    mu.say_level("The Bandit Chief bellows: \"Guards! To me!\"")
    mu.spawn{creature = "Brigand", near = m, count = 3, awake = true}
  end},
  {at = 35, run = function(m)
    mu.say_level("The Bandit Chief gulps down a potion and grins.")
    mu.heal(m, m.max_hp * 3 // 10)
    mu.spawn{creature = "Swordsman", near = m, count = 2, awake = true}
  end},
}

local function open_cell(d)
  if d.flags.cell then return end
  d.flags.cell = true
  for _, at in ipairs(d.places.D) do mu.tile(at, "'") end
  mu.say_level("[Fort] The Jailer's keys clatter to the floor. The prisoner's cell door swings open.")
end

mu.dungeon{
  key = KEY,
  name = "the Bandit Fort",
  label = "Bandit Fort",
  depth = 12,
  level = {10, 15},
  about = "a walled fort, a jail to break open, and the Bandit Chief.",
  map = MAP,
  legend = {
    D = "#",
    S = "$",
    b = {spawn = {creature = "Bandit"}},
    g = {spawn = {creature = "Brigand"}},
    m = {spawn = {creature = "Magic User", name = "Bandit Spellslinger"}},
    z = {item = "uncommon"},
  },
  build = function(d)
    local n = d.party
    mu.spawn{creature = "Swordsman", name = "Jailer", at = d.places.J[1], tag = "jailer", hp = 4}
    mu.spawn{creature = "Novice Warrior", name = "Captain Aldric", at = d.places.P[1], tag = "prisoner"}
    mu.spawn{creature = "Swordsman", name = "Bandit Chief", at = d.places.C[1], boss = "bandit_chief",
             hp = 10 * (1 + (n - 1) / 2), speed = 1}
    for _, at in ipairs(d.places.X) do mu.drop("uncommon", at) end -- the strongbox
    mu.drop("rare", d.places.X[2])
  end,
  enter = function(p, d)
    mu.tell(p, "[Fort] Smoke rises over a timber-and-stone fort. Somewhere inside, a prisoner is shouting for help.")
  end,
  talk = function(p, m, d)
    if m.tag ~= "prisoner" then return end
    if not d.flags.cell then
      mu.tell(p, "[Captain Aldric] \"Through the bars, friend! The Jailer has the keys. Kill him and get me out!\"")
      return
    end
    mu.say_level("[Captain Aldric] \"Free at last! Take this for your trouble. And listen: the chief keeps a strongbox"
                 .. " behind a loose stone in the east wall of his hall.\"")
    mu.tile(d.places.S[1], "'")
    for _, q in ipairs(mu.players()) do
      if mu.reward_once(q, "fort_prisoner") then
        mu.pay(q, 300)
        mu.tell(q, "[Fort] Captain Aldric gives you 300 gold.")
      end
    end
    mu.despawn(m)
    mu.say_level("[Fort] Captain Aldric slips out of the fort.")
  end,
  hurt = function(p, m, d)
    if m.boss == "bandit_chief" then mu.phases(m, CHIEF_PHASES) end
  end,
  kill = function(p, m, d)
    if m.tag == "jailer" then
      open_cell(d)
    elseif m.boss == "bandit_chief" then
      mu.say_level("[Fort] The Bandit Chief falls! The fort is yours.")
      mu.say_world(p.name .. (d.party > 1 and "'s party has" or " has") .. " taken the Bandit Fort!")
      mu.reward_party{key = KEY, title = TITLE, tier = "rare", gold = 500,
                      items = {{key = "bandit_kings_cloak", name = "the Cloak of the Bandit King", one_in = 3}}}
    end
  end,
}
