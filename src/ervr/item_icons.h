#pragma once
// Item icons for the weapon wheel and the belt, read from the game's own archive (game_archive.h) on a
// background thread: the icon's 1024 px BC7 texture is decoded and shrunk to 256 px, once per session.
// Nothing is written to disk, nothing waits for the game to show an icon first.
#include <cstdint>
#include <vector>

namespace ervr {

// Worker: asks for an icon (no-op when it is loaded, queued, or known to be missing). The first
// request starts the loader thread.
void ItemIconsRequest(int iconId);
// The icon if it has been read: at most 256 px a side, straight-alpha sRGB RGBA8 (r | g << 8 |
// b << 16 | a << 24). False while it loads, or when the archive has no such icon.
bool ItemIconRgba(int iconId, std::vector<uint32_t>& rgba, uint32_t& w, uint32_t& h);

struct ItemIconStats { long loaded = 0, failed = 0; };
ItemIconStats ItemIconsGetStats();

}
