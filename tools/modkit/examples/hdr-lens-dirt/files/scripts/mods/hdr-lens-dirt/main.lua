-- Lens dirt on the HDR image (slot 8): the bright parts of the picture light up the smudges of a dirty lens (hdr_lens_dirt.png).
dcmod.on_ready(function()
  dcmod.postfx.use("hdr_lens_dirt", 8)
  dcmod.postfx.param(0, 1.0, 8)    -- strength
  dcmod.postfx.param(1, 0.35, 8)   -- threshold
  dcmod.postfx.param(2, 0.18, 8)   -- spread
end)
