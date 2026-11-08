-- Volumetric sunlight on the HDR image (slot 8): shafts through the haze, cut by whatever stands between the air and the sun.
dcmod.on_ready(function()
  dcmod.postfx.use("hdr_volumetric", 8)
  dcmod.postfx.param(3, 0.012, 8)    -- scattering per metre (more is hazier)
  dcmod.postfx.param(7, 0.07, 8)     -- strength
  dcmod.postfx.param(8, 70, 8)       -- how far along each ray the light is gathered, metres
  dcmod.postfx.param(9, 0.65, 8)     -- how strongly the light scatters towards the sun (0 even, 0.9 a tight glow)
end)
local function dot(a, b) return a[1] * b[1] + a[2] * b[2] + a[3] * b[3] end
dcmod.every(0.03, function()
  local cam, sun = dcmod.camera(), dcmod.sun()
  if not cam or not sun then return end
  local d = sun.direction
  dcmod.postfx.param(0, dot(d, cam.right), 8)
  dcmod.postfx.param(1, dot(d, cam.up), 8)
  dcmod.postfx.param(2, -dot(d, cam.forward), 8)
  local peak = math.max(sun.colour[1], sun.colour[2], sun.colour[3], 1e-4)
  local fade = math.min(1, peak / 30)            -- no shafts at night
  dcmod.postfx.param(4, sun.colour[1] / peak * fade, 8)
  dcmod.postfx.param(5, sun.colour[2] / peak * fade, 8)
  dcmod.postfx.param(6, sun.colour[3] / peak * fade, 8)
end)
