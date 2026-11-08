-- A wet road on the HDR image (slot 8): damp asphalt with puddles whose reflections are sharp. The shader works in the world, so it
-- gets the camera's right and forward axes and its position every frame, then the four numbers below.
local WETNESS = 0.7       -- 0 dry .. 1 soaked: how much of the road is covered by puddles
local STRENGTH = 1.0      -- reflection strength
local DARKEN = 0.6        -- how dark damp asphalt gets (1: not at all)
local DAMP_BLUR = 0.5     -- how blurred the reflection on damp asphalt is
dcmod.on_ready(function() dcmod.postfx.use("hdr_wet_road", 8) end)
dcmod.every(0.03, function()
  local cam = dcmod.camera()
  if not cam then return end
  local r, f = cam.right, cam.forward
  local v = { r[1], r[2], r[3], f[1], f[2], f[3], cam.x, cam.y, cam.z, WETNESS, STRENGTH, DARKEN, DAMP_BLUR }
  for i = 1, #v do dcmod.postfx.param(i - 1, v[i], 8) end
end)
