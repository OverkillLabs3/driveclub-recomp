-- Thick volumetric fog on the HDR image (slot 8). The shader works in the world, so it gets the camera's place and axes and
-- the sun's direction every frame; the ground level of the fog is where the car's wheels are.
local DENSITY = 0.012      -- fog per metre at ground level (0.01 light mist, 0.03 thick, 0.06 very thick)
local FALLOFF = 0.060      -- per metre of height: 0.1 a thin carpet, 0.03 a tall bank
local THICKNESS = 1.0      -- a multiplier on the whole density
local ground = nil
dcmod.on_ready(function() dcmod.postfx.use("hdr_fog_volume", 8) end)
dcmod.every(0.03, function()
  local cam, sun = dcmod.camera(), dcmod.sun()
  if not cam or not sun then return end
  local car = dcmod.car()
  if car and car.position then ground = ground and (ground * 0.97 + car.position.y * 0.03) or car.position.y end
  local g = ground or (cam.y - 2)
  local r, f, d = cam.right, cam.forward, sun.direction
  local v = { r[1], r[2], r[3], f[1], f[2], f[3], cam.x, cam.y, cam.z, d[1], d[2], d[3], DENSITY, FALLOFF, g, THICKNESS }
  for i = 1, 16 do dcmod.postfx.param(i - 1, v[i], 8) end
end)
