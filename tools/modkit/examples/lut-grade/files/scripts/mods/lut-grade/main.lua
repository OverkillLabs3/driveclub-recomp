-- Colour grading with a lookup table (postfx/lut_grade.png) on the finished picture.
dcmod.on_ready(function()
  dcmod.postfx.use("lut_grade")
  dcmod.postfx.param(0, 1.0)   -- amount: 0 off .. 1 full
end)
