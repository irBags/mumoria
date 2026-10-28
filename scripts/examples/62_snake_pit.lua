-- The Snake Pit (levels 3-5): a raid for new characters.
--
-- Its gate (0) stands at 150 ft. A warm cave the game makes fresh for each
-- party, thick with snakes, frogs and centipedes; a Naga Brood-Guard, and
-- the Naga Mother coiled at the far end.

mu.raid{
  key = "snake_pit",
  name = "the Snake Pit",
  label = "Snake Pit",
  level = {3, 5},
  about = "Something has been breeding snakes down here. Its mother is bigger.",
  gate = 3,
  colour = 34,
  floors = {
    {depth = 3, generate = true,
     mobs = {"Large White Snake", "Large Brown Snake", "Large Black Snake", "Large Green Snake",
             "Giant Green Frog", "Metallic Green Centipede", "Giant Yellow Centipede"},
     packs = 7, pack = {2, 4},
     captains = 1, captain = {creature = "Black Naga", name = "Naga Brood-Guard", hp = 2}},
  },
  lord = {creature = "Green Naga", name = "Naga Mother", hp = 3,
          guard = {"Large Green Snake", "Large Black Snake", "Giant Green Frog"}},
  loot = {tier = "uncommon", count = 3},
  reward = {tier = "uncommon", gold = 100},
}
