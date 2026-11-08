-- Sun light shafts and lens ghosts on the HDR image (slot 5). The shader needs to know where the sun is: dcmod.sun() says.
dcmod.on_ready(function()
  dcmod.postfx.use("hdr_sunrays", 5)
  dcmod.postfx.param(4, 1.0, 5)    -- shaft tint red
  dcmod.postfx.param(5, 0.82, 5)   -- green
  dcmod.postfx.param(6, 0.55, 5)   -- blue
  dcmod.postfx.param(7, 0.96, 5)   -- decay: how far the shafts reach
  dcmod.postfx.param(8, 0.9, 5)    -- how much of the way to the sun is sampled
  dcmod.postfx.param(9, 1.0, 5)    -- lens ghosts
end)
dcmod.every(0.03, function()
  local sun = dcmod.sun()
  local on = sun and sun.screen[3] and sun.direction[2] > 0 and 1 or 0
  dcmod.postfx.param(0, sun and sun.screen[1] or 0.5, 5)
  dcmod.postfx.param(1, sun and sun.screen[2] or 0.5, 5)
  dcmod.postfx.param(2, on, 5)
  -- radiance 250 at noon is full strength; dusk is dimmer but the low sun makes the longest shafts
  local lum = sun and (0.3 * sun.colour[1] + 0.6 * sun.colour[2] + 0.1 * sun.colour[3]) or 0
  dcmod.postfx.param(3, math.min(1, lum / 120) * 0.9, 5)
end)
