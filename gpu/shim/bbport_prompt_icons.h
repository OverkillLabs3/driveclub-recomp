// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: keyboard and mouse, Xbox and Switch button prompts drawn in the style of the game's own
// key-guide icons (menu/common.tpf.dcx, KG_*: 32x32 BC7, stored in the PS4's tiled layout). Pure
// functions, no renderer state: bbport_prompts.cpp swaps them in, tests/test_prompt_icons.cpp
// renders them.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace BbPromptIcons {

inline constexpr int Size = 32;      ///< pixels per side, as the game's icons
inline constexpr int Bytes = 1024;   ///< 8x8 BC7 blocks of 16 bytes

/// The game's key-guide icons, named after their textures. A texture's name is its role in the
/// menus; the picture is a button (KG_OK shows Circle): ShownButton says which.
enum Icon : int {
    Cancel, L1, L2, L3, LS, L_D, L_LR, L_U, L_UD, L_UDLR, OK, R1, R2, R3, RS, R_L, R_U, Start, TP_L, TP_R,
    Count
};
extern const char* const Names[Count];
/// XXH3-64 of each icon as game 1.09 stores it (the texture cache's upload hash): how the
/// textures are recognised in guest memory.
extern const uint64_t PlayStationHash[Count];

/// What an icon shows, from the bindings: one entry per input it stands for (two for KG_L_UD,
/// four for a stick). An input is an input.ini name ("e", "space", "leftbutton",
/// "mousewheelup"), a combination joined with '+' ("lshift+leftbutton"), or "mouse" for
/// mouse motion. Empty: nothing is bound (the icon shows a dash).
using Inputs = std::vector<std::string>;

/// The TrueType font the labels are drawn with (the overlay's embedded DejaVu Sans). Call once
/// before Render; the data must stay valid.
bool SetFont(const unsigned char* ttf);

/// 32x32 RGBA, straight alpha.
using Pixels = std::array<uint8_t, Size * Size * 4>;
Pixels Draw(const Inputs& inputs);
/// BC7 (mode 6) in the game's tiled block order: the bytes a KG_* texture holds.
std::array<uint8_t, Bytes> Encode(const Pixels& pixels);
inline std::array<uint8_t, Bytes> Render(const Inputs& inputs) { return Encode(Draw(inputs)); }

/// The short label a key gets on its cap ("Space", "Shift", "E"); empty for a mouse input.
std::string KeyLabel(const std::string& input);

/// Other controllers' buttons in the place of the PlayStation ones, by position: Cross is the
/// bottom face button (Xbox A, Switch B), Circle the right one (Xbox B, Switch A).
enum Pad : int { PadXbox, PadSwitch, PadCount };
/// `icon` as `pad`'s button, in the game's style; false for an icon that stays the game's own
/// (the d-pad and the sticks, which every controller has alike).
bool DrawPad(Icon icon, Pad pad, Pixels& out);

/// The HUD atlas (MENU_Common_00091, 1024x512 BC7): its own Circle, Cross and L3, which the
/// prompts in the world use ("Pick Up Item"), not the KG_* icons. Each is a 26x26 sprite in a
/// block-aligned area that holds nothing else.
inline constexpr int HudAtlasWidth = 1024, HudAtlasHeight = 512;
enum HudSprite : int { HudCircle, HudCross, HudL3, HudCount };
struct Area { int x, y, w, h; };
extern const Area HudAreas[HudCount];
/// The key-guide icon whose button each sprite shows (its keyboard picture is the same).
extern const Icon HudIcon[HudCount];
/// XXH3-64 of each area's blocks as game 1.09 stores them, read row by row (HudBlocks).
extern const uint64_t HudPlayStationHash[HudCount];

/// Offset of BC7 block (bx, by) in a texture `width` pixels wide in the game's tiled layout:
/// 8x8-block micro tiles in rows, Morton order inside.
size_t TiledBlockOffset(int bx, int by, int width);
/// The sprite for `inputs` sized to `area` (the icon at 26x26, centred), as BC7 blocks row by row.
std::vector<uint8_t> RenderArea(const Inputs& inputs, const Area& area);
/// The same for an icon already drawn (DrawPad).
std::vector<uint8_t> RenderIconArea(const Pixels& icon, const Area& area);

/// DCport: DriveClub's button icons, each its own BC7 texture in the PS4's tiled layout (rows
/// and columns of blocks padded to whole 8x8-block micro tiles). Recognised by their upload
/// hash, as the key-guide icons are. The d-pad icons stay the game's own.
enum DcIcon : int {
    DcCross, DcCircle, DcSquare, DcTriangle, DcL1, DcR1, DcL2, DcR2, DcL3, DcR3, DcTouchpad,
    DcTouchpadSmall, DcCount
};
struct DcTexture { int width, height; uint64_t hash; };
extern const DcTexture DcTextures[DcCount];
/// The bytes of a DriveClub icon texture, padding included.
size_t DcBytes(DcIcon icon);
/// The looks DCport draws: Xbox 360 and Xbox One buttons (Cross is A, Circle B, Square X,
/// Triangle Y, the touchpad Back or View), and DualSense-style PlayStation symbols.
enum DcStyle : int { DcXbox360, DcXboxOne, DcPs5, DcStyleCount };
/// `icon` in `style`, drawn at the icon's size: straight RGBA, width x height.
std::vector<uint8_t> DrawDc(DcIcon icon, DcStyle style);
/// `rgba` (the icon's size) encoded into a copy of the game's texture `original` (DcBytes long):
/// the padding blocks keep the game's bytes.
std::vector<uint8_t> EncodeDc(DcIcon icon, const std::vector<uint8_t>& rgba,
                              const std::vector<uint8_t>& original);

} // namespace BbPromptIcons
