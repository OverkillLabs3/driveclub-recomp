-- Writes a line to the game log at every lap and once a minute. Look for lines that start with "Mod:".
dcmod.on("NewLap", function(...)
  dcmod.log("new lap, event arguments:", ...)
end)

local minutes = 0
dcmod.every(60, function()
  minutes = minutes + 1
  dcmod.log("the game has been running for about", minutes, "minutes")
end)
