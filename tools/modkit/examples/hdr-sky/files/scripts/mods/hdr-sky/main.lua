-- A physically based sky with clouds on the HDR image (slot 7), lit by the real sun (dcmod.sun) and following the camera.
dcmod.on_ready(function()
  dcmod.postfx.use("hdr_sky", 7)
end)
local coverage = 0.55   -- cloud cover, 0 clear to 1 overcast
local function dot(a, b) return a[1] * b[1] + a[2] * b[2] + a[3] * b[3] end
dcmod.every(0.03, function()
  local cam, sun = dcmod.camera(), dcmod.sun()
  if not cam or not sun then return end
  local d = sun.direction
  -- view space: x right, y up, z backward
  dcmod.postfx.param(0, dot(d, cam.right), 7)
  dcmod.postfx.param(1, dot(d, cam.up), 7)
  dcmod.postfx.param(2, -dot(d, cam.forward), 7)
  dcmod.postfx.param(3, coverage, 7)                   -- cloud cover, 0 clear to 1 overcast
  dcmod.postfx.param(4, cam.right[2], 7)               -- the world's up, in view space
  dcmod.postfx.param(5, cam.up[2], 7)
  dcmod.postfx.param(6, -cam.forward[2], 7)
  dcmod.postfx.param(7, cam.right[1], 7)               -- the camera's right, forward and up seen from above (x and z),
  dcmod.postfx.param(11, cam.right[3], 7)              -- to turn a view ray into a direction over the ground
  dcmod.postfx.param(12, cam.forward[1], 7)
  dcmod.postfx.param(13, cam.forward[3], 7)
  dcmod.postfx.param(14, cam.up[1], 7)
  dcmod.postfx.param(15, cam.up[3], 7)
  local peak = math.max(sun.colour[1], sun.colour[2], sun.colour[3], 1e-4)
  local fade = math.min(1, peak / 30)                  -- the sun fades out at night
  dcmod.postfx.param(8, sun.colour[1] / peak * fade, 7)
  dcmod.postfx.param(9, sun.colour[2] / peak * fade, 7)
  dcmod.postfx.param(10, sun.colour[3] / peak * fade, 7)
end)
