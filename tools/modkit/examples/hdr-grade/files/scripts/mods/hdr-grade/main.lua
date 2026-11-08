-- A cinematic grade on the HDR image (slot 3: before the game's own bloom and tone mapping).
dcmod.on_ready(function()
  dcmod.postfx.use("hdr_grade", 3)
  dcmod.postfx.param(0, 0.15, 3)    -- exposure in stops
  dcmod.postfx.param(1, 1.18, 3)    -- contrast
  dcmod.postfx.param(2, 1.12, 3)    -- saturation
  dcmod.postfx.param(3, 0.45, 3)    -- vignette
  dcmod.postfx.param(4, 0.06, 3)    -- grain
  dcmod.postfx.param(5, 0.55, 3)    -- split toning
end)
