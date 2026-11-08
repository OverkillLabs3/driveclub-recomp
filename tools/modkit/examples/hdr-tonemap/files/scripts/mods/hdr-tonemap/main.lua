-- Tone mapping curve on the HDR image (slot 6). F9 steps through the curves while racing: 0 none, 1 Reinhard, 2 ACES,
-- 3 Hable, 4 soft shoulder, 5 shows what is brighter than 1 in magenta. PageUp and PageDown change the exposure; the automatic
-- exposure adds to it (param 5, 0 turns it off).
local curve, exposure = 2, 1.0
local names = { [0] = "none", "Reinhard", "ACES", "Hable", "soft shoulder", "over 1 in magenta" }
local was = {}
local function once(key)
  local down = dcmod.key(key)
  local fire = down and not was[key]
  was[key] = down
  return fire
end
local function apply()
  dcmod.postfx.param(0, curve, 6)
  dcmod.postfx.param(1, exposure, 6)
  dcmod.postfx.param(2, 4.0, 6)    -- white point (Reinhard, Hable)
  dcmod.postfx.param(3, 1.0, 6)    -- saturation
  dcmod.postfx.param(4, 0.6, 6)    -- where the soft shoulder starts
  dcmod.postfx.param(5, 1.0, 6)    -- automatic exposure strength (0 off)
  dcmod.postfx.param(6, 0.2, 6)    -- the brightness automatic exposure aims for
  dcmod.postfx.param(7, 0.03, 6)   -- how fast the eye adapts, per frame (1: at once)
end
dcmod.on_ready(function()
  dcmod.postfx.use("hdr_tonemap", 6)
  apply()
end)
dcmod.every(0.05, function()
  if once("F9") then curve = (curve + 1) % 6 end
  if once(33) then exposure = exposure + 0.25 end
  if once(34) then exposure = exposure - 0.25 end
  apply()
  dcmod.ui.text("tm", 0.02, 0.9, string.format("tone map: %s   exposure %+.2f", names[curve], exposure), 18, { 1, 1, 1, 1 }, 0.2)
end)
