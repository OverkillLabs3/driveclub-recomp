-- A leaderboard of the race. dcmod.cars() gives each car's completed laps and its progress round the lap (0 to 1).
dcmod.every(0.25, function()
  local cars = dcmod.cars()
  if #cars == 0 then return end
  for _, c in ipairs(cars) do c.total = c.laps + c.progress end
  table.sort(cars, function(a, b) return a.total > b.total end)
  dcmod.ui.rect("board_bg", 0.74, 0.24, 0.25, 0.04 + 0.03 * #cars, { 0, 0, 0, 0.6 }, 0.6)
  for i, c in ipairs(cars) do
    local colour = c.player and { 1, 0.85, 0.2, 1 } or { 1, 1, 1, 1 }
    dcmod.ui.text("board_" .. i, 0.75, 0.25 + 0.03 * (i - 1), string.format("%d  %s  L%d %.0f%%", i, string.sub(c.name, 1, 22), c.laps + 1, c.progress * 100), 18, colour, 0.6)
  end
end)
