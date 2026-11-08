-- Switches the vignette filter on when the game is up. Change the numbers while playing: param 0 is the vignette
-- strength, param 1 the warm tint.
dcmod.on_ready(function()
  dcmod.postfx.use("vignette")
  dcmod.postfx.param(0, 0.8)
  dcmod.postfx.param(1, 0.35)
end)
