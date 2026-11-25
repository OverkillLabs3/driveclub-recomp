// Graphics detail the PS4 version keeps low, and the game's camera effects: an edited copy of the game's global.rpk
// with other values in its quality files (viewportquality.xml, vehiclelodgroupdata.xml).
#pragma once
#include <string>

struct GraphicsChoice {
  bool own_car = false;       // your car's full-detail model in races
  bool opponents = false;     // full-detail models for the other cars up close
  int scenery = 0;            // 0 game, 1 more detail further away (less pop-in), 2 photo mode's detail
  bool reflections = false;   // the higher-quality screen-space reflections of photo mode
  int shadows = 0;            // shadow edge filtering beyond 4 m: 0 game (8 samples), 1 16 samples, 2 32 samples
  bool particles = false;     // distant spray, smoke and dust kept, more particles
  bool dynamic_res = true;    // the game's dynamic resolution scaling (on in the game)
  bool motion_blur = true;    // the game's motion blur
  bool lens_dust = true;      // dirt on the camera lens against the sun
  bool heat_haze = true;      // heat shimmer over hot roads
  bool fxaa = true;           // the FXAA pass after the game's own anti-aliasing
  bool ssr = true;            // the screen-space reflections in races (wet roads, puddles)
  bool ssao = true;           // the screen-space ambient occlusion
  bool Default() const {
    return !own_car && !opponents && !scenery && !reflections && !shadows && !particles && dynamic_res &&
           motion_blur && lens_dust && heat_haze && fxaa && ssr && ssao;
  }
};

// Reads global.rpk from the player's own game archive and writes `out_dir`\global.rpk with the chosen values.
// The runtime serves it in place of the original (as a mods folder); the game folders are never touched. When
// every choice is at the game's default, the file is removed and nothing needs to be served (`active` false).
// Skips the work when the file in `out_dir` already matches the archive and the choices. Returns "" or a short
// description of a problem.
std::wstring BuildGraphics(const std::wstring& update_dir, const std::wstring& base_dir, const std::wstring& out_dir,
                           const GraphicsChoice& choice, bool& active);
