-- The Dungeon Warden. Copy this file into scripts/ with the dungeons
-- (51_ to 54_) to use them.
--
-- An old soldier stands in the town. Walk into him to talk: he lists every
-- dungeon a script has made (mu.dungeon), who it suits, and whether your
-- reward there is ready. Beside him, /delve <number> takes you in. Each
-- party gets a copy of its own: the others in your party /delve the same way
-- and join you inside. The way out ('0', where you came in) or /escape
-- brings you back.

local TAG = "warden"
local SPOT = {x = 33, y = 11} -- where he stands (or as near as is free)

mu.npc(TAG)

local function warden() return mu.tagged(TAG)[1] end

local function beside(p, m)
  return m and math.max(math.abs(p.x - m.x), math.abs(p.y - m.y)) <= 2
end

local function list(p)
  local lines = {"[Warden] \"Delvers, eh? These are open to you:\""}
  for i, d in ipairs(mu.dungeons()) do
    local lv = d.level and string.format(" (levels %d-%d)", d.level[1], d.level[2]) or ""
    lines[#lines + 1] = string.format("  %d) %s%s: %s", i, d.name, lv, d.about or "")
  end
  lines[#lines + 1] = "  Say /delve <number> beside me. Rewards come once every 12 hours each."
  for _, line in ipairs(lines) do mu.tell(p, line) end
end

mu.every(5, function(p, level)
  if not warden() then
    mu.spawn{creature = "Veteran", name = "Dungeon Warden", at = SPOT, tag = TAG}
  end
end, 0)

mu.on("talk", function(p, m)
  if m.tag == TAG then list(p) end
end)

mu.command("delve", function(p, args)
  local m = warden()
  if p.depth ~= 0 or not beside(p, m) then
    mu.tell(p, "[Warden] Find the Dungeon Warden in the town, and stand beside him.")
    return
  end
  local all = mu.dungeons()
  local d = all[tonumber(args) or 0]
  if not d then
    list(p)
    return
  end
  if d.level and p.level < d.level[1] then
    mu.tell(p, string.format("[Warden] \"%s? At level %d? Come back at %d.\"", d.name, p.level, d.level[1]))
    return
  end
  mu.tell(p, "[Warden] \"Good hunting.\" He unlocks a heavy door and waves you through.")
  mu.enter_dungeon(p, d.key)
end, "go into a dungeon (beside the Dungeon Warden)")

mu.command("escape", function(p)
  if not mu.leave_dungeon(p) then mu.tell(p, "You are not in a dungeon.") end
end, "out of the dungeon you are in, to where you came in from")
