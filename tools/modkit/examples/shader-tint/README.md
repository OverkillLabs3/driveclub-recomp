# shader-tint

An example of changing shaders: `tint.py` reads the shaders dumped by the game, changes every fragment shader that writes
a colour so that its green and blue outputs are 0, and writes the result as a mod's `files/shaders` folder.

```
set DC_DUMP_SHADERS=1        (delete data\user\cache first, then start the game and drive a lap)
python tint.py data\user\shader\dumps mymod\files\shaders
dcmod mod new mymod --name "Red world"
```

Put `mymod` in the launcher's Mods page. The game then draws in one colour. The shaders are not included here: they are
the game's own and are made by the game on your machine. See docs/modding/shaders.md.
