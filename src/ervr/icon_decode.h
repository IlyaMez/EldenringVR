#pragma once
// Item icons as ELDEN RING ships them: one DDS texture inside a TPF container (menu/hi/00_solo
// holds MENU_Knowledge_<iconId>.tpf.dcx files, EquipParamWeapon.iconId names the entry). This is
// the pure half - container walk and block decoding, no game or GPU state - so the regression test
// can check it against a reference decoder (tools/test_icon_decode.cpp, golden data from Pillow).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ervr {

enum class DdsKind { Unknown = 0, BC1, BC2, BC3, BC7, RGBA8, BGRA8 };

struct DdsInfo {
    DdsKind  kind = DdsKind::Unknown;
    uint32_t width = 0, height = 0, mips = 0;
    uint32_t dxgi = 0;          // DX10 header's DXGI_FORMAT (0 = legacy FourCC / mask header)
    uint32_t fourcc = 0;        // legacy pixel-format FourCC (0 = none)
    size_t   dataOffset = 0;    // where mip 0 starts
    size_t   mip0Size = 0;      // bytes of mip 0
};

// Header only. True when the file is a DDS whose mip 0 lies wholly inside [p, p + n); `out` is
// filled as far as it could be read either way (kind Unknown = a format this decoder lacks).
bool DdsParse(const uint8_t* p, size_t n, DdsInfo& out);
const char* DdsKindName(DdsKind k);

// Mip 0 as straight-alpha RGBA8, row-major, one uint32 per pixel = r | g << 8 | b << 16 | a << 24.
// The colour stays in the texture's own encoding (sRGB for the menu icons).
bool DdsDecode(const uint8_t* p, size_t n, std::vector<uint32_t>& rgba, uint32_t& w, uint32_t& h);

// One 4x4 block -> 16 pixels, same packing.
void DecodeBC1Block(const uint8_t* block, uint32_t out[16], bool threeColorMode);
void DecodeBC2Block(const uint8_t* block, uint32_t out[16]);
void DecodeBC3Block(const uint8_t* block, uint32_t out[16]);
void DecodeBC7Block(const uint8_t* block, uint32_t out[16]);

// A texture inside a TPF. `dds` points into the caller's buffer, and is null while the texture's data
// does not lie wholly inside it yet: the game decompresses a big file in steps, so the first piece
// holds the header and the names but only the start of a 1 MB icon. `dataEnd` is where it ends.
struct TpfTexture { std::string name; const uint8_t* dds = nullptr; size_t ddsLen = 0; size_t dataEnd = 0; };
// The PC-platform TPF's textures whose names lie inside the buffer. False when the buffer is not a
// TPF at all. Defensive by design: it reads whatever a game file loader produced.
bool TpfTextures(const uint8_t* p, size_t n, std::vector<TpfTexture>& out);

// "MENU_Knowledge_01234" -> 1234, anything else -> -1.
int MenuIconId(const std::string& textureName);

// Box-filters straight-alpha RGBA8 down until neither side exceeds `maxSide` (premultiplied while
// averaging, so transparent texels do not bleed dark fringes in). Smaller images are copied as is.
void ShrinkRgba(const std::vector<uint32_t>& src, uint32_t w, uint32_t h, uint32_t maxSide,
                std::vector<uint32_t>& out, uint32_t& ow, uint32_t& oh);
// An uncompressed RGBA8 DDS file (legacy header) of the pixels: the icon cache's format.
std::vector<uint8_t> MakeRgbaDds(const std::vector<uint32_t>& px, uint32_t w, uint32_t h);

} // namespace ervr
