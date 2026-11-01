#include "icon_decode.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace ervr;
namespace {
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}
// splitmix64, the generator the golden hashes were made with.
struct SplitMix {
    uint64_t s;
    uint64_t Next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    void Fill(uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; i += 8) { const uint64_t v = Next(); std::memcpy(p + i, &v, n - i < 8 ? n - i : 8); }
    }
};
struct Fnv {
    uint64_t h = 0xCBF29CE484222325ull;
    void Px(uint32_t v) { for (int c = 0; c < 4; ++c) { h ^= (v >> (8 * c)) & 0xFFu; h *= 0x100000001B3ull; } }
};
void Put32(std::vector<uint8_t>& b, size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
std::vector<uint8_t> MakeDds(const std::vector<uint8_t>& blocks, uint32_t w, uint32_t h, bool dx10, const char* fourcc) {
    std::vector<uint8_t> d(128 + (dx10 ? 20 : 0));
    std::memcpy(d.data(), "DDS ", 4);
    Put32(d, 4, 124); Put32(d, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000); Put32(d, 12, h); Put32(d, 16, w);
    Put32(d, 20, (uint32_t)blocks.size()); Put32(d, 28, 1); Put32(d, 76, 32); Put32(d, 80, 0x4);
    std::memcpy(d.data() + 84, dx10 ? "DX10" : fourcc, 4);
    Put32(d, 108, 0x1000);
    if (dx10) { Put32(d, 128, 98); Put32(d, 132, 3); Put32(d, 140, 1); }
    d.insert(d.end(), blocks.begin(), blocks.end());
    return d;
}
}

// Bit-exact against Pillow's BCn decoder (golden hashes from the same splitmix64 stream; see the
// generator in the 2026-11-01 weapon-wheel session notes). A mismatch means a table, a bit order
// or an interpolation rule is wrong - the menu icons would come out speckled.
int main() {
    SplitMix rng{0x45525652ull};
    const uint64_t bc7[8] = {0x7E9C33971F62316Eull, 0x23477740A7E21346ull, 0xE9ADA6D613E690C7ull, 0x3BED61C377277F88ull,
                             0x14AD1D8219AA36FEull, 0xC8A57137B8ACC6F1ull, 0xC8F623932BB0694Bull, 0x9AECE91B8AFBB37Aull};
    for (int mode = 0; mode < 8; ++mode) {
        Fnv f;
        for (int k = 0; k < 64; ++k) {
            uint8_t b[16]; rng.Fill(b, 16);
            b[0] = (uint8_t)((b[0] & ~((1u << (mode + 1)) - 1u)) | (1u << mode));
            uint32_t px[16]; DecodeBC7Block(b, px);
            for (uint32_t v : px) f.Px(v);
        }
        if (f.h != bc7[mode]) { std::fprintf(stderr, "BC7 mode %d: 0x%016llX\n", mode, (unsigned long long)f.h); Check(false, "BC7 decode differs from the reference"); }
    }
    {
        Fnv f;
        for (int k = 0; k < 64; ++k) { uint8_t b[8]; rng.Fill(b, 8); uint32_t px[16]; DecodeBC1Block(b, px, true); for (uint32_t v : px) f.Px(v); }
        if (f.h != 0x006113E43194658Full) std::fprintf(stderr, "BC1: 0x%016llX\n", (unsigned long long)f.h);
        Check(f.h == 0x006113E43194658Full, "BC1 (DXT1) decode differs from the reference");
    }
    {
        Fnv f;
        for (int k = 0; k < 64; ++k) { uint8_t b[16]; rng.Fill(b, 16); uint32_t px[16]; DecodeBC3Block(b, px); for (uint32_t v : px) f.Px(v); }
        if (f.h != 0x6A938DD8FE0EE219ull) std::fprintf(stderr, "BC3: 0x%016llX\n", (unsigned long long)f.h);
        Check(f.h == 0x6A938DD8FE0EE219ull, "BC3 (DXT5) decode differs from the reference");
    }
    {
        Fnv f;
        for (int k = 0; k < 64; ++k) { uint8_t b[16]; rng.Fill(b, 16); uint32_t px[16]; DecodeBC2Block(b, px); for (uint32_t v : px) f.Px(v); }
        if (f.h != 0xE3B7689B54EBA421ull) std::fprintf(stderr, "BC2: 0x%016llX\n", (unsigned long long)f.h);
        Check(f.h == 0xE3B7689B54EBA421ull, "BC2 (DXT3) decode differs from the reference");
    }
    {
        uint8_t reserved[16] = {};
        uint32_t px[16]; DecodeBC7Block(reserved, px);
        bool zero = true; for (uint32_t v : px) zero = zero && v == 0;
        Check(zero, "a reserved-mode BC7 block decodes to transparent black");
    }
    // A whole DDS file, 6x6 so the right and bottom blocks are clipped.
    std::vector<uint8_t> blocks(64);
    rng.Fill(blocks.data(), blocks.size());
    const std::vector<uint8_t> dds = MakeDds(blocks, 6, 6, true, nullptr);
    DdsInfo info;
    Check(DdsParse(dds.data(), dds.size(), info) && info.kind == DdsKind::BC7 && info.width == 6 && info.height == 6 &&
          info.dataOffset == 148 && info.mip0Size == 64, "a DX10 BC7 header must parse");
    Check(!DdsParse(dds.data(), dds.size() - 1, info), "a DDS whose mip 0 is cut short must be refused");
    std::vector<uint32_t> rgba; uint32_t w = 0, h = 0;
    Check(DdsDecode(dds.data(), dds.size(), rgba, w, h) && w == 6 && h == 6, "the 6x6 DDS must decode");
    Fnv img; for (uint32_t v : rgba) img.Px(v);
    if (img.h != 0xDACD5A825F00E9BFull) std::fprintf(stderr, "6x6: 0x%016llX\n", (unsigned long long)img.h);
    Check(img.h == 0xDACD5A825F00E9BFull, "a clipped BC7 image must match the reference row for row");

    // TPF: one UTF-16-named texture, the way the menu icon files carry it.
    std::vector<uint8_t> tpf(16 + 20, 0);
    std::memcpy(tpf.data(), "TPF", 4);
    Put32(tpf, 8, 1); tpf[12] = 0; tpf[14] = 1;
    const char* name = "MENU_Knowledge_01234";
    const uint32_t nameOff = (uint32_t)tpf.size();
    for (const char* c = name; ; ++c) { tpf.push_back((uint8_t)*c); tpf.push_back(0); if (!*c) break; }
    while (tpf.size() % 16) tpf.push_back(0);
    const uint32_t ddsOff = (uint32_t)tpf.size();
    tpf.insert(tpf.end(), dds.begin(), dds.end());
    Put32(tpf, 4, (uint32_t)dds.size());
    Put32(tpf, 16, ddsOff); Put32(tpf, 20, (uint32_t)dds.size()); tpf[24] = 102; tpf[26] = 1;
    Put32(tpf, 28, nameOff); Put32(tpf, 32, 0);
    std::vector<TpfTexture> tex;
    Check(TpfTextures(tpf.data(), tpf.size(), tex) && tex.size() == 1, "a one-texture TPF must list its texture");
    Check(tex[0].name == name && tex[0].dds == tpf.data() + ddsOff && tex[0].ddsLen == dds.size(), "the TPF entry must point at its DDS");
    // The first piece of a file the game decompresses in steps: names yes, texture data not yet.
    Check(TpfTextures(tpf.data(), ddsOff + 64, tex) && tex.size() == 1 && tex[0].name == name && !tex[0].dds &&
          tex[0].dataEnd == ddsOff + dds.size(), "a TPF's first piece names its icon and says where its data ends");
    Check(MenuIconId(tex[0].name) == 1234, "MENU_Knowledge_01234 names icon 1234");
    Check(MenuIconId("MENU_Knowledge_") == -1 && MenuIconId("MENU_ItemIcon_00001") == -1 && MenuIconId("MENU_Knowledge_0012a") == -1,
          "only MENU_Knowledge_<digits> is an item icon");
    Check(!TpfTextures(dds.data(), dds.size(), tex), "a DDS is not a TPF");
    tpf[16 + 16] = 7;   // an unknown entry layout: stop, never misread
    Check(TpfTextures(tpf.data(), tpf.size(), tex) && tex.empty(), "an entry layout the walk does not know yields nothing");

    // The cache keeps icons small: 1024 px game icons shrink to 128, alpha-weighted, and round-trip a DDS.
    {
        std::vector<uint32_t> big((size_t)256 * 256, 0), small;
        for (uint32_t y = 0; y < 256; ++y)
            for (uint32_t x = 0; x < 256; ++x)
                big[(size_t)y * 256 + x] = (x & 1) ? 0x00000000u : 0xFF2040C0u;   // opaque blue-ish beside transparent black
        uint32_t ow = 0, oh = 0;
        ShrinkRgba(big, 256, 256, 128, small, ow, oh);
        Check(ow == 128 && oh == 128, "256 px shrinks to 128");
        Check((small[0] & 0x00FFFFFFu) == 0x2040C0u && (small[0] >> 24) == 128,
              "transparent texels thin the alpha without darkening the colour");
        const std::vector<uint8_t> file = MakeRgbaDds(small, ow, oh);
        std::vector<uint32_t> back; uint32_t bw = 0, bh = 0;
        Check(DdsDecode(file.data(), file.size(), back, bw, bh) && bw == 128 && bh == 128 && back == small,
              "the cache's RGBA DDS reads back exactly");
        std::vector<uint32_t> same; ShrinkRgba(small, 128, 128, 128, same, ow, oh);
        Check(same == small && ow == 128, "an icon already small enough is kept as is");
    }
    std::puts("icon decode: ok");
    return 0;
}
