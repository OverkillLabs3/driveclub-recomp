// The cockpit camera's head (data/cameras/vehicle/driver_head_cameras/include/driver_head_camera_params.txt): the game
// ships everything needed for the head to look into corners and to sway with braking and cornering, but keeps it
// at 0. An edited copy of the player's own file, served from a mods folder, turns it on.
#pragma once
#include <string>

struct HeadCamChoice {
  int look = 0;       // %: the head turns toward where the car is going (0 = the game, 100 = fully into the direction of travel)
  int weight = 0;     // %: softer neck and head springs, the head sways with braking and cornering (100 = x0.25)
  bool Default() const { return !look && !weight; }
};

// The file's text with the chosen values: only the numbers of the keys the choices touch change; every other
// byte (comments, tabs, CRLF, the IsHMD block) stays as it is. All at 0 returns the text unchanged.
std::string HeadCamApply(const std::string& text, const HeadCamChoice& choice);

// Reads the file from the player's own game archive and writes it, with the choices applied, under `out_dir`
// (as a mods folder: out_dir\data\cameras\...). When every choice is at the game's default the file is removed
// and nothing needs to be served (`active` false). Returns "" or a short description of a problem.
std::wstring BuildHeadCam(const std::wstring& update_dir, const std::wstring& base_dir, const std::wstring& out_dir,
                          const HeadCamChoice& choice, bool& active);
