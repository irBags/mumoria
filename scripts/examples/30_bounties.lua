-- Example events and a command. Copy this file into scripts/ to use it.
--
-- Orc bounties: every 25 orcs a character kills earns a Rare piece and a
-- shout to the world. /bounty shows how many they have. mu.set / mu.get
-- keep the count in the database, so it survives restarts.

local EVERY = 25

local function count_key(p)
  return "bounty:" .. p.name
end

mu.on("kill", function(p, m)
  if m.letter ~= "o" then return end
  local n = tonumber(mu.get(count_key(p)) or "0") + 1
  mu.set(count_key(p), n)
  if n % EVERY == 0 then
    mu.drop("rare", m)
    mu.say_world("[Bounty] " .. p.name .. " has slain " .. n .. " orcs!")
  end
end)

mu.command("bounty", function(p, args)
  local n = tonumber(mu.get(count_key(p)) or "0")
  mu.tell(p, "[Bounty] You have slain " .. n .. " orcs; the next reward at " .. (n // EVERY + 1) * EVERY .. ".")
end, "your orc bounty")

mu.on("enter", function(p, level)
  if level.depth == 1 and p.level == 1 then
    mu.tell(p, "[Bounty] The town pays for orcs: every " .. EVERY .. " you slay earns a reward. /bounty counts them.")
  end
end)
