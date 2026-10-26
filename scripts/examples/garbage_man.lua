-- The Garbage Man. Copy this file into scripts/ to use it.
--
-- Every half hour he comes out and walks the town, taking every item left
-- on the ground 5 minutes or more, then goes home. Every blow heals him, so
-- he cannot be killed by hitting him; if something does get rid of him, the
-- next round starts as usual. While this script is loaded the game's own
-- 30-minute clearing of the town is off: he does the job instead.

local ROUND = 30 * 60   -- seconds from one round to the next
local LEFT_FOR = 5 * 60 -- an item must lie this long before he takes it
local STEPS = 2         -- squares a second
local PATIENCE = 60     -- seconds he tries for one item (someone may stand on it)
local TAG = "garbage_man"

mu.hold(TAG)            -- he walks where this script says, and attacks nobody
mu.town_expiry(false)

local next_round = tonumber(mu.get("garbage_man:next") or "0")
local target, since, skip = nil, 0, {}

local function key(c) return c.x .. "," .. c.y end

-- the nearest item he should take, or nil
local function next_item(m)
  local best, best_d
  for _, it in ipairs(mu.items()) do
    if it.age >= LEFT_FOR and not skip[key(it)] then
      local d = math.max(math.abs(it.x - m.x), math.abs(it.y - m.y))
      if not best or d < best_d then best, best_d = it, d end
    end
  end
  return best
end

local function start_round()
  next_round = mu.now() + ROUND
  mu.set("garbage_man:next", next_round)
  target, skip = nil, {}
  local any = false
  for _, it in ipairs(mu.items()) do
    if it.age >= LEFT_FOR then any = true break end
  end
  if not any then return nil end
  local m = mu.spawn{creature = "Pitiful-Looking Beggar", name = "Garbage Man", far = true, hp = 30000, awake = true, tag = TAG}
  if m then mu.say_level("The Garbage Man trundles out with his cart to tidy the streets.") end
  return m
end

mu.every(1, function(p, level)
  local m = mu.tagged(TAG)[1]
  if not m then
    target = nil
    if mu.now() < next_round then return end
    m = start_round()
    if not m then return end
  end
  if target and mu.now() - since > PATIENCE then
    skip[key(target)] = true -- out of reach for now: the next round tries again
    target = nil
  end
  if not target then
    target = next_item(m)
    since = mu.now()
    if not target then
      mu.say_level("The Garbage Man wheels his cart home.")
      mu.despawn(m)
      return
    end
  end
  local at = mu.walk(m, target, STEPS)
  if not at then
    skip[key(target)] = true
    target = nil
  elseif at.x == target.x and at.y == target.y then
    mu.take(target) -- nil if someone picked it up first
    target = nil
  end
end, 0)

mu.on("hurt", function(p, m)
  if m.tag ~= TAG then return end
  mu.heal(m, 30000)
  mu.tell(p, "The Garbage Man shrugs it off and gets on with his work.")
end)
