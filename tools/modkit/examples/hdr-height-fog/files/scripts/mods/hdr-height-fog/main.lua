-- Height fog with sun scattering on the HDR image (slot 8). The shader needs the world's up in view space, the camera's
-- height and the sun's direction and colour.
local ground = nil   -- the height (world y) where the fog is at its full density: where the car's wheels are
dcmod.on_ready(function()
  dcmod.postfx.use("hdr_height_fog", 8)
  dcmod.postfx.param(7, 0.0035, 8)   -- fog per metre at ground level
  dcmod.postfx.param(11, 0.012, 8)   -- falloff per metre of height
  dcmod.postfx.param(12, 0.9, 8)     -- sun glow in the fog
  dcmod.postfx.param(13, 0.45, 8)    -- brightness of the fog colour
end)
local function dot(a, b) return a[1] * b[1] + a[2] * b[2] + a[3] * b[3] end
dcmod.every(0.03, function()
  local cam, sun = dcmod.camera(), dcmod.sun()
  if not cam or not sun then return end
  dcmod.postfx.param(0, cam.right[2], 8)
  dcmod.postfx.param(1, cam.up[2], 8)
  dcmod.postfx.param(2, -cam.forward[2], 8)
  local car = dcmod.car()
  if car and car.position then ground = ground and (ground * 0.95 + car.position.y * 0.05) or car.position.y end
  dcmod.postfx.param(3, cam.y - (ground or cam.y - 2), 8)
  local d = sun.direction
  dcmod.postfx.param(4, dot(d, cam.right), 8)
  dcmod.postfx.param(5, dot(d, cam.up), 8)
  dcmod.postfx.param(6, -dot(d, cam.forward), 8)
  local peak = math.max(sun.colour[1], sun.colour[2], sun.colour[3], 1e-4)
  dcmod.postfx.param(8, sun.colour[1] / peak, 8)
  dcmod.postfx.param(9, sun.colour[2] / peak, 8)
  dcmod.postfx.param(10, sun.colour[3] / peak, 8)
end)
