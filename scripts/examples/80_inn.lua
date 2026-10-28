-- The Rusty Tankard: an inn in the town, to hang out, drink and sleep.
--
-- The script builds the inn in the town's east margin, below the Auction
-- House, its door an amber 0. Inside is one inn for everyone (a shared
-- dungeon, where nobody can fight): a common room with a bar, tables and a
-- hearth, a sleeping hall, a kitchen. Walk into the innkeeper for the menu;
-- walk into the furniture to see what it does.
--
--   /drink ale|wine|mead|spirits   a drink: each does something, too many makes the room spin
--   /eat                           a bowl of stew: fed, and a little better
--   /sleep                         beside a bed: whole again, cured, and well rested (blessed)
--   /round                         a round of ale for everyone in the inn
--   /toast <words>                 raise a mug to something

local KEY = "inn"
local DOOR = {x = 57, y = 16}               -- in the town; the building is 57-63 x 14-18
local NAME = "the Rusty Tankard"

mu.npc("inn_keeper", "I")
mu.npc("inn_bard", "B")
mu.npc("inn_patron", "p")
mu.npc("inn_cook", "c")

local MAP = [[
##########################################################
##########################################################
##K.....I...........................####...............###
##KUUUUUUUUUUUU.....................####.B...........B.###
##K.........................b.......####...............###
##..................................####...............###
##.............................hhh..####.B...........B.###
##........tt.....tt.....tt..........####...............###
##..................................,',,...............###
##.................p................####...............###
##..................................####.B...........B.###
##........tt.....tt.....tt..........####...............###
##...........q......................####...............###
##..<.@...........................N.####.B...........B.###
##..................................####...............###
##################'#######################################
##################,#######################################
##############..........##################################
##############..O....c..##################################
##############..........##################################
##########################################################
##########################################################
]]

local DRINKS = {
  ale = {price = 4, booze = 1, say = "a foaming mug of ale", hp = 15},
  wine = {price = 12, booze = 1, say = "a cup of red wine", hp = 25, mana = 25},
  mead = {price = 20, booze = 2, say = "a horn of honeyed mead", hp = 10, buff = {"bless", 150},
          feel = "Warm and golden: you feel blessed."},
  spirits = {price = 35, booze = 3, say = "a shot of dwarven firewater", buff = {"hero", 150}, buff2 = {"resist_cold", 300},
             feel = "Fire in your belly! You feel like a hero, and the cold cannot touch you."},
}

local SONGS = {
  "The bard sings of the Kobold Chief, who lost his trousers in his own den.",
  "The bard plays a slow air about the Old Crypt, and the room goes quiet.",
  "The bard strikes up a reel. Someone at the back starts stamping.",
  "The bard sings of the Naga Queen and her drowned temple, far below.",
  "The bard sings a rude verse about orcs. The room roars.",
  "The bard tells of a Giant King who sits on a cloud, deep under the earth.",
  "The bard tunes the lute, badly, and blames the damp.",
  "The bard sings of heroes who went down and never came back up. Cheerful stuff.",
}

local GOSSIP = {
  "\"Kobolds under the town, I tell you. Their gate's at fifty feet, plain as day.\"",
  "\"Never go down alone. Even the Warden says so.\"",
  "\"My cousin went into the Orc Stronghold. Three floors! He came back with a cleaver.\"",
  "\"The old crypt's awake again. Bring light, and a friend.\"",
  "\"A raid's reward comes once every twelve hours. Plan your nights.\"",
  "\"A scroll of recall's worth more than a sword, down there.\"",
  "\"The firewater here could strip paint. Try it.\"",
}

local booze = {}                            -- by name: {n, at}

local function drunk(p)
  local b = booze[p.name]
  if not b then return 0 end
  b.n = math.max(0, b.n - (mu.now() - b.at) // 120) -- sobering, a drink every two minutes
  b.at = mu.now()
  return b.n
end

local function inside(p)
  local level = mu.level()
  if not (level and level.dungeon == KEY) then
    mu.tell(p, "[Inn] You need to be in " .. NAME .. " (the amber 0 in the town).")
    return false
  end
  return true
end

local function pay(p, price, what)
  if p.gold < price then
    mu.tell(p, "[Inn] " .. what .. " is " .. price .. " gold, and you have " .. p.gold .. ".")
    return false
  end
  mu.pay(p, -price)
  return true
end

local function menu(p)
  mu.tell(p, "[Innkeeper] \"What'll it be?\"")
  mu.tell(p, "  /drink ale 4g (a little better)    /drink wine 12g (hit points and mana)")
  mu.tell(p, "  /drink mead 20g (blessed)          /drink spirits 35g (heroic, and warm)")
  mu.tell(p, "  /eat 8g (stew)   /sleep beside a bed, 10g + 2g a level   /round   /toast <words>")
end

local function beside(p, places)
  for _, at in ipairs(places or {}) do
    if math.max(math.abs(p.x - at.x), math.abs(p.y - at.y)) <= 1 then return true end
  end
  return false
end

local function bed_price(p) return 10 + 2 * p.level end

local function notices(p)
  mu.tell(p, "[Notice board] Raids, pinned up by the town watch:")
  local any = false
  for _, d in ipairs(mu.dungeons()) do
    if d.kind == "raid" then
      any = true
      local where = d.gate and (d.gate == 0 and "its gate in the town" or ("its gate at " .. d.gate * 50 .. " ft")) or "ask the Warden"
      local floors = d.floors > 1 and (", " .. d.floors .. " floors") or ""
      mu.tell(p, string.format("  %s (levels %d-%d%s): %s.", d.name, d.level[1], d.level[2], floors, where))
    end
  end
  if not any then mu.tell(p, "  Nothing yet.") end
  mu.tell(p, "  And the old raid gates: every fifth level, from 250 ft down.")
end

local function spawn_npcs(d)
  local function one(letter, creature, name, tag)
    local at = d.places[letter] and d.places[letter][1]
    if at then mu.spawn{creature = creature, name = name, at = at, tag = tag} end
  end
  one("I", "Battle-Scarred Veteran", "Innkeeper", "inn_keeper")
  one("b", "Singing, Happy Drunk", "Bard", "inn_bard")
  one("p", "Squint-Eyed Rogue", "Card Player", "inn_patron")
  one("q", "Mean-Looking Mercenary", "Old Mercenary", "inn_patron")
  one("c", "Pitiful-Looking Beggar", "Cook", "inn_cook")
end

-- the building in the town, a block of wall like the shops'; its walls are
-- drawn again each time someone comes into the town (cheap, and it mends a
-- building drawn by an older script), its door once
mu.on("enter", function(p, level)
  if level.depth ~= 0 or level.dungeon then return end
  local whole = true
  for y = 14, 18 do
    for x = 57, 63 do
      if not (x == DOOR.x and y == DOOR.y) and not mu.tile({x = x, y = y}, "=") then whole = false end
    end
  end
  if whole and not mu.find_portal(KEY) and mu.tile(DOOR, ".") then
    mu.portal{key = KEY, at = DOOR, glyph = "0", colour = 214}
  end
end)

mu.on("portal", function(p, key)
  if key == KEY then mu.enter_dungeon(p, KEY) end
end)

mu.dungeon{
  key = KEY,
  name = NAME,
  label = "Rusty Tankard",
  depth = 1,
  shared = true,                            -- one inn for everyone
  safe = true,                              -- nobody fights here
  hidden = true,                            -- the Warden does not list it
  lit = true,                               -- all of it seen at once: nothing to explore
  map = MAP,
  legend = {
    U = {decor = "=", name = "the bar", colour = 130},
    K = {decor = "&", name = "a keg", colour = 94},
    t = {decor = "=", name = "a table", colour = 137},
    h = {decor = "*", name = "the hearth", colour = 202},
    N = {decor = "?", name = "notice board", colour = 230},
    B = {decor = "=", name = "a bed", colour = 67},
    O = {decor = "*", name = "the oven", colour = 166},
  },
  build = spawn_npcs,
  every = 15,
  enter = function(p, d)
    mu.tell(p, "[Inn] Warmth, pipe smoke and the smell of stew. Walk into the innkeeper (I) for a drink.")
    local here = {}
    for _, o in ipairs(mu.players()) do
      if o.name ~= p.name then here[#here + 1] = o.name end
    end
    if #here > 0 then mu.tell(p, "[Inn] Here already: " .. table.concat(here, ", ") .. ".") end
    mu.say_level("[Inn] " .. p.name .. " comes in from the cold.")
  end,
  talk = function(p, m, d)
    if m.tag == "inn_keeper" then
      menu(p)
    elseif m.tag == "inn_bard" then
      mu.tell(p, "[Bard] \"Requests? I know them all. Badly.\" " .. SONGS[mu.random(#SONGS)])
    elseif m.tag == "inn_cook" then
      mu.tell(p, "[Cook] \"Stew's hot. /eat, eight gold. Don't ask what's in it.\"")
    else
      mu.tell(p, "[" .. m.name .. "] " .. GOSSIP[mu.random(#GOSSIP)])
    end
  end,
  bump = function(p, d, letter)
    if letter == "B" then
      mu.tell(p, "[Inn] A straw bed with a clean blanket. /sleep: " .. bed_price(p) .. " gold for the night.")
    elseif letter == "N" then
      notices(p)
    elseif letter == "h" then
      mu.tell(p, "[Inn] You warm your hands at the hearth.")
    elseif letter == "U" then
      menu(p)
    elseif letter == "K" then
      mu.tell(p, "[Inn] A keg of the house ale. The innkeeper is watching you.")
    elseif letter == "O" then
      mu.tell(p, "[Inn] The oven is hot enough to bake a troll.")
    else
      return false
    end
  end,
  tick = function(p, d)
    local r = mu.random(6)
    if r <= 2 and #mu.tagged("inn_bard") > 0 then
      mu.say_level("[Inn] " .. SONGS[mu.random(#SONGS)])
    elseif r == 3 then
      mu.say_level("[Inn] The Old Mercenary mutters: " .. GOSSIP[mu.random(#GOSSIP)])
    end
  end,
}

mu.command("menu", function(p)
  if inside(p) then menu(p) end
end, "the inn's drinks and food (in the inn)")

mu.command("drink", function(p, args)
  if not inside(p) then return end
  local what = (args or ""):lower():match("%a+") or "ale"
  local d = DRINKS[what]
  if not d then
    mu.tell(p, "[Innkeeper] \"We've ale, wine, mead and spirits.\"")
    return
  end
  if drunk(p) >= 8 then
    mu.tell(p, "[Innkeeper] \"You've had enough, friend. Sit down before you fall down.\"")
    return
  end
  if not pay(p, d.price, (what:gsub("^%l", string.upper))) then return end
  local b = booze[p.name] or {n = 0, at = mu.now()}
  booze[p.name] = b
  b.n = drunk(p) + d.booze
  mu.tell(p, "[Inn] You drink " .. d.say .. ".")
  local whole = {}
  if d.hp then whole.hp = p.max_hp * d.hp // 100 + 1 end
  if d.mana then whole.mana = p.max_mana * d.mana // 100 + 1 end
  mu.restore(p, whole)
  if d.buff then mu.buff(p, d.buff[1], d.buff[2]) end
  if d.buff2 then mu.buff(p, d.buff2[1], d.buff2[2]) end
  if d.feel then mu.tell(p, "[Inn] " .. d.feel) end
  if b.n >= 5 then
    mu.buff(p, "confused", 2 + b.n)
    mu.tell(p, "[Inn] The room tilts pleasantly. Then less pleasantly.")
    if b.n >= 7 then mu.say_level("[Inn] " .. p.name .. " is singing louder than the bard.") end
  end
end, "a drink at the inn: ale, wine, mead, spirits")

mu.command("eat", function(p)
  if not inside(p) or not pay(p, 8, "Stew") then return end
  mu.restore(p, {food = "full", hp = p.max_hp // 10 + 1})
  mu.tell(p, "[Inn] A bowl of thick, brown stew. You are full, and feel better for it.")
end, "a bowl of stew at the inn")

mu.command("sleep", function(p)
  if not inside(p) then return end
  -- the beds: the B squares of the inn's map (row 1 is y = 1)
  local beds = {}
  local row = 0
  for line in MAP:gmatch("[^\n]+") do
    row = row + 1
    for col = 1, #line do
      if line:sub(col, col) == "B" then beds[#beds + 1] = {x = col, y = row} end
    end
  end
  if not beside(p, beds) then
    mu.tell(p, "[Inn] Find a bed in the sleeping hall (east), and stand beside it.")
    return
  end
  if not pay(p, bed_price(p), "A bed") then return end
  booze[p.name] = nil
  mu.restore(p, {hp = "full", mana = "full", cure = true})
  mu.buff(p, "bless", 300)
  mu.tell(p, "[Inn] You sleep like a stone and wake whole, clear-headed and well rested (blessed).")
  mu.say_level("[Inn] " .. p.name .. " rents a bed and is soon snoring.")
end, "sleep in an inn bed (beside one)")

mu.command("round", function(p)
  if not inside(p) then return end
  local all = mu.players()
  if not pay(p, 4 * #all, "A round for " .. #all) then return end
  for _, o in ipairs(all) do
    mu.restore(o, {hp = o.max_hp * 15 // 100 + 1})
    if o.name ~= p.name then mu.tell(o, "[Inn] " .. p.name .. " buys you an ale!") end
  end
  mu.say_level("[Inn] " .. p.name .. " buys a round for the house! Cheers all round.")
end, "a round of ale for everyone in the inn")

mu.command("toast", function(p, args)
  if not inside(p) then return end
  local words = (args or ""):sub(1, 80)
  mu.say_level("[Inn] " .. p.name .. " raises a mug" .. (words ~= "" and (": \"" .. words .. "!\"") or "!") .. " Mugs clatter.")
end, "raise a mug at the inn")
