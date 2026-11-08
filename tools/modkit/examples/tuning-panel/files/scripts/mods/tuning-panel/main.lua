-- F8 shows or hides the panel; click and drag with the mouse.
local hour = 12
dcmod.gui.app("Tuning", function(g)
  local now = dcmod.time()
  g:text(now and string.format("clock %.1f h", now) or "no race")
  local new_hour = g:slider("Hour", hour, 0, 24)
  if math.abs(new_hour - hour) > 0.01 then hour = new_hour; dcmod.set_time(hour) end
  if g:button("Midnight") then hour = 0; dcmod.set_time(0) end
  local car = dcmod.car()
  g:text(car and string.format("%.0f km/h, gear %d", car.kmh, car.gear) or "no car")
end)
