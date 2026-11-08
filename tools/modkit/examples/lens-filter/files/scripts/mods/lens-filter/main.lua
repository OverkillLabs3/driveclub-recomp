-- Lens effects on the finished picture: chromatic aberration, barrel distortion, sharpen, grain.
dcmod.on_ready(function()
  dcmod.postfx.use("lens")
  dcmod.postfx.param(0, 0.0015)   -- chromatic aberration
  dcmod.postfx.param(1, 0.02)    -- barrel distortion
  dcmod.postfx.param(2, 0.35)    -- sharpen
  dcmod.postfx.param(3, 0.025)   -- grain
end)
