-- A bloom with an anamorphic glare, made of five chained slots of one shader (hdr_bloom.frag): every slot reads what the
-- one before drew, and the last one also reads the untouched scene. The sizes make the chain cheap.
local function stage(slot, size, mode, a, b, c)
  dcmod.postfx.use("hdr_bloom", slot)
  dcmod.postfx.scale(size, slot)
  dcmod.postfx.param(0, mode, slot)
  dcmod.postfx.param(1, a or 0, slot)
  dcmod.postfx.param(2, b or 0, slot)
  dcmod.postfx.param(3, c or 0, slot)
end

dcmod.on_ready(function()
  stage(3, 0.5, 0, 0.4, 0.3)             -- bright pass above 0.4 (scene units) with a soft knee, half size
  stage(4, 0.25, 1)                      -- blur, quarter size
  stage(5, 0.125, 1)                     -- blur, eighth size
  stage(6, 0.25, 2)                      -- upsample
  stage(7, 1.0, 3, 1.0, 0.6, 6.0)        -- composite: bloom strength, glare strength, glare length
end)
