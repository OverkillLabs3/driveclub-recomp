dcmod.every(0.1, function()
  local car = dcmod.car()
  if not car then return end
  dcmod.ui.rect("bg", 0.40, 0.82, 0.20, 0.12, { 0, 0, 0, 0.55 }, 0.5)
  dcmod.ui.text("kmh", 0.42, 0.83, "Speed " .. math.floor(car.kmh) .. " km/h", 44, { 1, 0.8, 0, 1 }, 0.5)
  dcmod.ui.text("gear", 0.42, 0.89, "Gear " .. car.gear .. "  " .. math.floor(car.rpm) .. " rpm", 26, { 1, 1, 1, 1 }, 0.5)
  dcmod.ui.rect("rpm", 0.42, 0.935, 0.16 * (car.rpm / car.max_rpm), 0.012, { 1, 0.2, 0.1, 1 }, 0.5)
end)
