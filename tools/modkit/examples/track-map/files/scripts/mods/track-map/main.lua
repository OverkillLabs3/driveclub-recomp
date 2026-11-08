-- A track map that draws itself: a dot every 25 m of driving, scaled to fit a box in the top left, and the car as a bigger dot.
local points, last = {}, nil
local box = { x = 0.03, y = 0.18, w = 0.16, h = 0.28 }   -- fractions of the screen
dcmod.every(0.1, function()
  local car = dcmod.car()
  if not car then points, last = {}, nil return end
  local p = car.position
  if not last or math.abs(p.x - last.x) + math.abs(p.z - last.z) > 25 then
    if #points < 150 then points[#points + 1] = { x = p.x, z = p.z } end
    last = { x = p.x, z = p.z }
  end
  local minx, maxx, minz, maxz = p.x, p.x, p.z, p.z
  for _, q in ipairs(points) do
    minx, maxx, minz, maxz = math.min(minx, q.x), math.max(maxx, q.x), math.min(minz, q.z), math.max(maxz, q.z)
  end
  local scale = math.min(box.w / math.max(maxx - minx, 1), box.h / math.max(maxz - minz, 1))
  local function at(q) return box.x + (q.x - minx) * scale, box.y + (maxz - q.z) * scale end   -- north (+z) is up
  dcmod.ui.rect("map_bg", box.x - 0.01, box.y - 0.01, box.w + 0.02, box.h + 0.02, { 0, 0, 0, 0.5 }, 0.4)
  for i, q in ipairs(points) do
    local x, y = at(q)
    dcmod.ui.rect("map_" .. i, x, y, 0.004, 0.007, { 1, 1, 1, 0.9 }, 0.4)
  end
  local cx, cy = at(p)
  dcmod.ui.rect("map_car", cx - 0.003, cy - 0.005, 0.008, 0.014, { 1, 0.8, 0.1, 1 }, 0.4)
end)
