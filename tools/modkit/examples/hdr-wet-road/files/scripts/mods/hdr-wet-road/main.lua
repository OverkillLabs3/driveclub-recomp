-- Wet road look with screen space reflections on the HDR image (slot 8). The shader needs the world's up in view space.
dcmod.on_ready(function()
  dcmod.postfx.use("hdr_wet_road", 8)
  dcmod.postfx.param(3, 0.9, 8)     -- wetness 0 .. 1
  dcmod.postfx.param(4, 0.6, 8)     -- reflection strength
  dcmod.postfx.param(5, 0.6, 8)    -- how much the wet ground darkens (1: not at all)
  dcmod.postfx.param(6, 1.0, 8)     -- roughness: how blurred the reflections are (0 mirror)
end)
dcmod.every(0.03, function()
  local cam = dcmod.camera()
  if not cam then return end
  dcmod.postfx.param(0, cam.right[2], 8)
  dcmod.postfx.param(1, cam.up[2], 8)
  dcmod.postfx.param(2, -cam.forward[2], 8)
end)
