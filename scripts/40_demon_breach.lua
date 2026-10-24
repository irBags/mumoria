-- The Demon Breach: a raid in the town. Copy this file into scripts/ to use it.
--
-- Now and then, when enough strong players stand in the town, the ground
-- shakes and a minute later a breach (a red 0) tears open somewhere in the
-- streets. Waves of demons pour out of it; then two Pit Fiends; then
-- the Demon Lord Azgaroth, who calls more of the Abyss as he weakens.
-- Slay him and the breach closes: everyone in the town who stood their
-- ground gets an Epic piece (sometimes Legendary, sometimes Breachbane), once
-- every few hours. Hold out too long, or leave the town, and the breach
-- closes on its own and drags its demons back.
--
-- The demons scale to the strong players in the town: three rosters, by
-- their average level. Umoria has few demons, so each is a creature given a
-- demon's name (its letter on the map stays the creature's). The game says
-- "The <name>", so names read best without their own "the" or a comma.
--
-- Commands: /breach (how it stands). Admins: /breach start | stop | ready.

local CFG = {
  key = "breach",            -- the portal's key
  glyph = "0", colour = 196, -- red
  every_hours = 4,           -- at most this often by itself
  chance_minutes = 15,       -- once due: on average this many minutes of champions in town
  champion_level = 12,       -- a champion: a player at least this level
  champions = 2,             -- this many in town for it to start by itself
  newcomer_level = 10,       -- warned to keep away
  warning_seconds = 60,
  waves = 5,
  wave_seconds = 30,
  fiend_seconds = 180,       -- the Pit Fiends' time before their lord comes anyway
  trickle_seconds = 20,      -- while the fiends or the lord live, more imps this often
  time_limit_minutes = 25,
  abandoned_seconds = 120,   -- no champion in town this long: it closes
  reward_hours = 12,
  reward_level = 5,          -- a reward needs this level at least
}

local ROSTERS = {
  {upto = 19,
   minions = {{name = "Imp", creature = "Black Harpy", hp = 2}, {name = "Hellspawn", creature = "Hobgoblin", hp = 2}},
   fiend = {name = "Pit Fiend", creature = "Fire Giant", hp = 3},
   lord = {name = "Demon Lord Azgaroth", creature = "Umber Hulk", hp = 12, speed = 1}},
  {upto = 29,
   minions = {{name = "Imp", creature = "Fire Spirit", hp = 1}, {name = "Hellspawn", creature = "Troll", hp = 2},
              {name = "Fiend", creature = "Fire Giant", hp = 1}},
   fiend = {name = "Pit Fiend", creature = "Giant Troll", hp = 4},
   lord = {name = "Demon Lord Azgaroth", creature = "Fire Elemental", hp = 8, speed = 1}},
  {upto = 999,
   minions = {{name = "Imp", creature = "Fire Spirit", hp = 2}, {name = "Fiend", creature = "Fire Giant", hp = 2},
              {name = "Hellspawn", creature = "Two-Headed Troll", hp = 2}},
   fiend = {name = "Pit Fiend", creature = "Fire Elemental", hp = 4},
   lord = {name = "Demon Lord Azgaroth", creature = "Fire Elemental", hp = 16, speed = 2}},
}

mu.item{
  key = "breachbane",
  name = "Breachbane",
  base = "Bastard Sword",
  tier = "legendary",
  to_hit = 12, to_dam = 14,
  dice = "4d5",
  powers = {"slay_evil", "resist_fire", "see_invisible", "regeneration", "con"},
  bonus = 2,
  from = "never",            -- only from the breach
}

-- tags: the minions, the fiends and the lord (the last two drop a boss's loot)
local DEMON, FIEND, LORD = "breach_imp", "breach_fiend", "breach_lord"

local S = {stage = "idle"}  -- this run of the server; the cool-down is kept in the database

local function champions()
  local out = {}
  for _, p in ipairs(mu.players()) do
    if p.level >= CFG.champion_level then out[#out + 1] = p end
  end
  return out
end

local function roster_for(list)
  local sum = 0
  for _, p in ipairs(list) do sum = sum + p.level end
  local avg = #list > 0 and sum // #list or CFG.champion_level
  for _, r in ipairs(ROSTERS) do
    if avg <= r.upto then return r end
  end
  return ROSTERS[#ROSTERS]
end

local function say(text) mu.say_level("[Breach] " .. text) end
local function shout(text) mu.say_world("[Breach] " .. text) end

-- always a list (count given), maybe empty
local function spawn(spec, near, count, tag, boss)
  return mu.spawn{creature = spec.creature, name = spec.name, near = near, count = count, hp = spec.hp or 1,
                  speed = spec.speed or 0, awake = true, tag = (not boss) and tag or nil, boss = boss and tag or nil}
end

local function spew(count)
  local portal = mu.find_portal(CFG.key)
  if not portal then return 0 end
  local made = 0
  for _ = 1, count do
    local spec = S.roster.minions[mu.random(#S.roster.minions)]
    made = made + #spawn(spec, portal, 1, DEMON)
  end
  mu.glow(portal, "legendary")
  return made
end

local function clear_demons()
  for _, tag in ipairs({DEMON, FIEND, LORD}) do
    for _, m in ipairs(mu.tagged(tag)) do mu.despawn(m) end
  end
end

local function finish(cooldown)
  clear_demons()
  mu.close_portal(CFG.key)
  if cooldown then mu.set("breach:next", mu.now() + CFG.every_hours * 3600) end
  S = {stage = "idle", checked = true}
end

local function begin(level_players)
  S = {stage = "warning", checked = true, roster = roster_for(level_players), open_at = mu.now() + CFG.warning_seconds,
       deadline = mu.now() + CFG.warning_seconds + CFG.time_limit_minutes * 60, tries = 0}
  shout("The ground beneath the town shakes. Something is tearing its way through. Champions, to the town!")
  say("A breach will open in the streets in " .. CFG.warning_seconds .. " seconds. Those below level " ..
      CFG.newcomer_level .. ", keep well clear of it!")
end

local function rewards()
  local now = mu.now()
  local got = {}
  for _, p in ipairs(mu.players()) do
    local key = "breach:reward:" .. p.name
    local last = tonumber(mu.get(key) or "0")
    if p.level < CFG.reward_level then
      mu.tell(p, "[Breach] You lived through it. Come back stronger for a share of the spoils.")
    elseif now - last < CFG.reward_hours * 3600 then
      local left = CFG.reward_hours * 3600 - (now - last)
      mu.tell(p, "[Breach] Your share of the spoils comes again in " .. (left + 3599) // 3600 .. "h.")
    else
      local what = mu.random(20) == 1 and "breachbane" or mu.random(10) == 1 and "legendary" or "epic"
      if mu.give(p, what) then
        mu.set(key, now)
        got[#got + 1] = p.name
        mu.tell(p, "[Breach] For holding the town, a prize from Azgaroth's hoard is yours (see your pack).")
      else
        mu.tell(p, "[Breach] Your pack is full: your prize falls at your feet.")
        mu.drop(what, p)
        mu.set(key, now)
      end
    end
  end
  return got
end

-- the breach's clock: town ticks only
mu.every(1, function(p, level)
  local now = mu.now()
  if not S.checked then
    -- after a restart or /reload: a breach left half done is closed
    S.checked = true
    if mu.find_portal(CFG.key) or #mu.tagged(DEMON) + #mu.tagged(FIEND) + #mu.tagged(LORD) > 0 then
      finish(false)
      say("The last embers of the breach gutter out.")
    end
  end

  if S.stage == "idle" then
    local due = tonumber(mu.get("breach:next") or "0")
    local list = champions()
    if now >= due and #list >= CFG.champions and mu.random(CFG.chance_minutes * 60) == 1 then
      begin(list)
    end
    return
  end

  if #champions() > 0 then
    S.empty_since = nil
  else
    S.empty_since = S.empty_since or now
  end
  if now >= S.deadline or (S.empty_since and now - S.empty_since >= CFG.abandoned_seconds) then
    finish(true)
    shout("The breach in the town seals itself, and drags its demons back into the dark. The Abyss will try again.")
    return
  end

  if S.stage == "warning" then
    if now >= S.open_at then
      local at = mu.portal{key = CFG.key, glyph = CFG.glyph, colour = CFG.colour}
      if at then
        S.stage, S.wave, S.next_wave = "waves", 0, now
        shout("A breach has torn open in the town! Demons pour out of it!")
        mu.glow(at, "legendary")
      else
        S.tries = S.tries + 1
        if S.tries > 10 then finish(false) end
      end
    end

  elseif S.stage == "waves" then
    if now >= S.next_wave then
      S.wave = S.wave + 1
      if S.wave > CFG.waves then
        local portal = mu.find_portal(CFG.key)
        for _ = 1, 2 do
          spawn(S.roster.fiend, portal, 1, FIEND, true)
          spew(2)
        end
        S.stage, S.fiend_deadline, S.next_trickle = "fiends", now + CFG.fiend_seconds, now + CFG.trickle_seconds
        say("Two Pit Fiends stride out of the breach!")
      else
        local n = math.min(18, 2 + 2 * #champions() + S.wave)
        spew(n)
        S.next_wave = now + CFG.wave_seconds
        say("Wave " .. S.wave .. " of " .. CFG.waves .. ": " .. n .. " demons claw their way out!")
      end
    end

  elseif S.stage == "fiends" or S.stage == "lord" then
    if now >= S.next_trickle then
      spew(2)
      S.next_trickle = now + CFG.trickle_seconds
    end
    if S.stage == "fiends" and (#mu.tagged(FIEND) == 0 or now >= S.fiend_deadline) then
      S.stage = "lord"
    end
    if S.stage == "lord" and not mu.find_boss(LORD) then
      -- he comes (or, if the town lost him somehow, comes again)
      local lord = spawn(S.roster.lord, mu.find_portal(CFG.key), 1, LORD, true)[1]
      if lord then
        shout(lord.name .. " steps through the breach into the town!")
      end
    end
  end
end, 0)

-- the lord's fight: at 75, 50 and 25 percent
local PHASES = {
  {at = 75, say = "Azgaroth roars, and the breach flares: more demons spill out!", spew = 6},
  {at = 50, say = "The breach pours its fire into its master! His wounds close, and a Pit Fiend answers his call!", heal = 5, fiend = true},
  {at = 25, say = "In his fury Azgaroth calls the whole Abyss!", spew = 10},
}

mu.on("hurt", function(p, m)
  if m.boss ~= LORD or S.stage ~= "lord" then return end
  local pct = m.hp * 100 // math.max(1, m.max_hp)
  local phase = PHASES[m.phase + 1]
  while phase and pct <= phase.at do
    m.phase = m.phase + 1
    mu.set_phase(m, m.phase)
    say(phase.say)
    if phase.spew then spew(phase.spew) end
    if phase.heal then mu.heal(m, m.max_hp // phase.heal) end
    if phase.fiend then spawn(S.roster.fiend, m, 1, FIEND, true) end
    phase = PHASES[m.phase + 1]
  end
end)

mu.on("kill", function(p, m)
  if m.boss == FIEND then
    local left = #mu.tagged(FIEND) -- (the one dying is no longer counted)
    say(p.name .. " has slain a Pit Fiend!" .. (S.stage == "fiends" and left > 0 and " One remains." or ""))
  elseif m.boss == LORD and S.stage == "lord" then
    mu.drop("epic", m)
    mu.drop("epic", m)
    S.stage = "won"
    local got = rewards()
    finish(true)
    shout(p.name .. " has slain the Demon Lord Azgaroth! The breach collapses and the town is saved." ..
          (#got > 0 and " Rewarded: " .. table.concat(got, ", ") .. "." or ""))
  end
end)

mu.on("portal", function(p, key)
  if key ~= CFG.key then return end
  mu.tell(p, "[Breach] The breach's fire hurls you back! Nothing living may pass it.")
  local at = mu.find_portal(CFG.key)
  if at then mu.teleport(p, {x = at.x + 4, y = at.y}) end
end)

mu.on("enter", function(p, level)
  if level.depth ~= 0 or S.stage == "idle" then return end
  if p.level < CFG.newcomer_level then
    mu.tell(p, "[Breach] The town is under attack by demons! Keep away from the red breach (0) and its spawn.")
  else
    mu.tell(p, "[Breach] Demons are pouring into the town through a breach. Champions are needed!")
  end
end)

mu.command("breach", function(p, args)
  local now = mu.now()
  if p.admin and args == "start" then
    if p.depth ~= 0 then mu.tell(p, "[Breach] Only in the town.") return end
    if S.stage ~= "idle" then mu.tell(p, "[Breach] One is already under way.") return end
    local list = champions()
    begin(#list > 0 and list or {p})
    return
  elseif p.admin and args == "stop" then
    if p.depth ~= 0 then mu.tell(p, "[Breach] Only in the town.") return end
    finish(false)
    say("An admin seals the breach.")
    return
  elseif p.admin and args == "ready" then
    mu.set("breach:next", 0)
    mu.tell(p, "[Breach] Ready: it may open whenever " .. CFG.champions .. " champions are in the town.")
    return
  end
  if S.stage == "idle" then
    local due = tonumber(mu.get("breach:next") or "0")
    mu.tell(p, "[Breach] The town is quiet." .. (now < due and " The Abyss gathers its strength (" .. (due - now + 3599) // 3600 .. "h)." or
        " But the Abyss stirs: it may break through when " .. CFG.champions .. " champions of level " .. CFG.champion_level .. "+ stand in the town."))
  elseif S.stage == "warning" then
    mu.tell(p, "[Breach] The ground shakes: the breach opens in " .. math.max(0, S.open_at - now) .. "s.")
  elseif S.stage == "waves" then
    mu.tell(p, "[Breach] Wave " .. S.wave .. " of " .. CFG.waves .. "; " .. #mu.tagged(DEMON) .. " demons in the town.")
  elseif S.stage == "fiends" then
    mu.tell(p, "[Breach] " .. #mu.tagged(FIEND) .. " Pit Fiends stand; their lord comes in " .. math.max(0, S.fiend_deadline - now) .. "s.")
  else
    local lord = mu.find_boss(LORD)
    mu.tell(p, "[Breach] Azgaroth is in the town" .. (lord and (" (" .. lord.hp * 100 // math.max(1, lord.max_hp) .. "%)") or "") .. "!")
  end
end, "how the town's breach stands")
