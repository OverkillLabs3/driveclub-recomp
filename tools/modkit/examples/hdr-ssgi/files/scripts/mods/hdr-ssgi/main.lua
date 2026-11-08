-- Ambient occlusion and a bounce of light from the screen, smoothed over time, on the HDR image (slot 8).
dcmod.on_ready(function()
  dcmod.postfx.use("hdr_ssgi", 8)
  dcmod.postfx.param(0, 2.4, 8)    -- radius in metres
  dcmod.postfx.param(1, 1.0, 8)    -- occlusion strength
  dcmod.postfx.param(2, 0.8, 8)    -- bounce light
  dcmod.postfx.param(3, 0.3, 8)   -- how much of the new frame goes into the running value (smoothing over time)
end)
