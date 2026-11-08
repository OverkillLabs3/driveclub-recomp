-- Depth effects on the HDR image (slot 4): ambient occlusion, depth of field and distance fog.
dcmod.on_ready(function()
  dcmod.postfx.use("hdr_depth_fx", 4)
  dcmod.postfx.param(0, 0.0016, 4)  -- fog density per metre
  dcmod.postfx.param(1, 0.75, 4)    -- fog strength
  dcmod.postfx.param(2, 0.0, 4)     -- fog on the sky
  dcmod.postfx.param(3, 0, 4)       -- 0 normal, 1 distance view, 2 normals view
  dcmod.postfx.param(4, 1.0, 4)     -- fog tint red (the fog takes the colour of the sky)
  dcmod.postfx.param(5, 0.97, 4)    -- green
  dcmod.postfx.param(6, 0.9, 4)     -- blue
  dcmod.postfx.param(7, 1.0, 4)     -- fog brightness
  dcmod.postfx.param(8, 0.9, 4)     -- ambient occlusion strength
  dcmod.postfx.param(9, 1.2, 4)     -- occlusion radius in metres
  dcmod.postfx.param(10, 0.35, 4)   -- depth of field strength
  dcmod.postfx.param(11, 1.0, 4)    -- depth of field amount
  dcmod.postfx.param(12, 0.0, 4)    -- focus distance (0: the car)
end)
