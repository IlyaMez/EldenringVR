#include "icon_decode.h"
#include <algorithm>
#include <cstring>

namespace ervr {
namespace {

// BC7 partition and anchor tables (BPTC: the D3D11 and Khronos data-format specs carry the same
// ones). Two subsets: bit i set = pixel i is in subset 1. Three subsets: two bits per pixel, pixel
// i at bits 2i. Read back from Pillow's decoder with crafted blocks and checked bit-exact against
// it on random blocks of every mode (tools/test_icon_decode.cpp).
constexpr uint16_t kPart2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8,
    0xFF00, 0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110,
    0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696,
    0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720,
    0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22};
constexpr uint32_t kPart3[64] = {
    0xAA685050, 0x6A5A5040, 0x5A5A4200, 0x5450A0A8, 0xA5A50000, 0xA0A05050, 0x5555A0A0, 0x5A5A5050,
    0xAA550000, 0xAA555500, 0xAAAA5500, 0x90909090, 0x94949494, 0xA4A4A4A4, 0xA9A59450, 0x2A0A4250,
    0xA5945040, 0x0A425054, 0xA5A5A500, 0x55A0A0A0, 0xA8A85454, 0x6A6A4040, 0xA4A45000, 0x1A1A0500,
    0x0050A4A4, 0xAAA59090, 0x14696914, 0x69691400, 0xA08585A0, 0xAA821414, 0x50A4A450, 0x6A5A0200,
    0xA9A58000, 0x5090A0A8, 0xA8A09050, 0x24242424, 0x00AA5500, 0x24924924, 0x24499224, 0x50A50A50,
    0x500AA550, 0xAAAA4444, 0x66660000, 0xA5A0A5A0, 0x50A050A0, 0x69286928, 0x44AAAA44, 0x66666600,
    0xAA444444, 0x54A854A8, 0x95809580, 0x96969600, 0xA85454A8, 0x80959580, 0xAA141414, 0x96960000,
    0xAAAA1414, 0xA05050A0, 0xA0A5A5A0, 0x96000000, 0x40804080, 0xA9A8A9A8, 0xAAAAAA44, 0x2A4A5254};
// The pixel whose index drops its top bit, for subset 1 of a two-subset block and subsets 1 and 2
// of a three-subset one (subset 0's anchor is always pixel 0).
constexpr uint8_t kAnchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,
    8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,
    15, 15, 15, 15, 15, 2,  2,  15};
constexpr uint8_t kAnchor3a[64] = {
    3,  3,  15, 15, 8,  3,  15, 15, 8,  8,  6,  6,  6,  5,  3,  3,  3,  3,  8,  15, 3,  3,  6,  10, 5,  8,  8,  6,
    8,  5,  15, 15, 8,  15, 3,  5,  6,  10, 8,  15, 15, 3,  15, 5,  15, 15, 15, 15, 3,  15, 5,  5,  5,  8,  5,  10,
    5,  10, 8,  13, 15, 12, 3,  3};
constexpr uint8_t kAnchor3b[64] = {
    15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,  15, 8,  3,  15, 6,  10,
    15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15, 3,  6,  6,  8,  15, 3,  15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 3,  15, 15, 8};
constexpr uint8_t kW2[4] = {0, 21, 43, 64};
constexpr uint8_t kW3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr uint8_t kW4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

// Per mode: subsets, partition / rotation / index-selection bits, colour and alpha endpoint bits,
// P-bits (0 none, 1 one per endpoint, 2 one per subset), primary and secondary index bits.
struct Bc7Mode { uint8_t subsets, partBits, rotBits, selBits, colorBits, alphaBits, pBits, idxBits, idx2Bits; };
constexpr Bc7Mode kModes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 3, 0}, {2, 6, 0, 0, 6, 0, 2, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 2, 0}, {2, 6, 0, 0, 7, 0, 1, 2, 0},
    {1, 0, 2, 1, 5, 6, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 2, 2}, {1, 0, 0, 0, 7, 7, 1, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 2, 0}};

struct Bits {
    const uint8_t* b; unsigned pos;
    uint32_t Get(unsigned n) {
        uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i, ++pos) v |= (uint32_t)((b[pos >> 3] >> (pos & 7)) & 1u) << i;
        return v;
    }
};

uint8_t Expand(uint32_t v, unsigned bits) {
    const uint32_t s = v << (8 - bits);
    return (uint8_t)(s | (s >> bits));
}
const uint8_t* Weights(unsigned bits) { return bits == 2 ? kW2 : bits == 3 ? kW3 : kW4; }
uint8_t Lerp64(uint8_t a, uint8_t b, uint8_t w) { return (uint8_t)(((64u - w) * a + w * b + 32u) >> 6); }
uint32_t Pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | (g << 8) | (b << 16) | (a << 24); }
uint16_t U16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
constexpr uint32_t FourCC(char a, char b, char c, char d) { return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) | ((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24); }

// The BC1 palette. BC2/BC3 colour blocks always interpolate four colours; only a BC1 block whose
// first endpoint is not the larger one switches to three colours plus transparent black.
void Bc1Palette(const uint8_t* block, uint32_t pal[4], bool threeColorMode) {
    const uint16_t c0 = U16(block), c1 = U16(block + 2);
    uint32_t r[4], g[4], b[4];
    auto unpack = [](uint16_t c, uint32_t& r, uint32_t& g, uint32_t& b) {
        r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31;
        r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
    };
    unpack(c0, r[0], g[0], b[0]); unpack(c1, r[1], g[1], b[1]);
    pal[0] = Pack(r[0], g[0], b[0], 255); pal[1] = Pack(r[1], g[1], b[1], 255);
    if (!threeColorMode || c0 > c1) {
        pal[2] = Pack((2 * r[0] + r[1]) / 3, (2 * g[0] + g[1]) / 3, (2 * b[0] + b[1]) / 3, 255);
        pal[3] = Pack((r[0] + 2 * r[1]) / 3, (g[0] + 2 * g[1]) / 3, (b[0] + 2 * b[1]) / 3, 255);
    } else {
        pal[2] = Pack((r[0] + r[1]) / 2, (g[0] + g[1]) / 2, (b[0] + b[1]) / 2, 255);
        pal[3] = 0;
    }
}
void Bc1Colors(const uint8_t* block, uint32_t out[16], bool threeColorMode) {
    uint32_t pal[4]; Bc1Palette(block, pal, threeColorMode);
    const uint32_t idx = U32(block + 4);
    for (int i = 0; i < 16; ++i) out[i] = pal[(idx >> (2 * i)) & 3];
}
void SetAlpha(uint32_t& px, uint32_t a) { px = (px & 0x00FFFFFFu) | (a << 24); }

} // namespace

const char* DdsKindName(DdsKind k) {
    switch (k) {
    case DdsKind::BC1: return "BC1"; case DdsKind::BC2: return "BC2"; case DdsKind::BC3: return "BC3";
    case DdsKind::BC7: return "BC7"; case DdsKind::RGBA8: return "RGBA8"; case DdsKind::BGRA8: return "BGRA8";
    default: return "unknown";
    }
}

void DecodeBC1Block(const uint8_t* block, uint32_t out[16], bool threeColorMode) { Bc1Colors(block, out, threeColorMode); }

void DecodeBC2Block(const uint8_t* block, uint32_t out[16]) {
    Bc1Colors(block + 8, out, false);
    for (int i = 0; i < 16; ++i) SetAlpha(out[i], ((block[i >> 1] >> ((i & 1) * 4)) & 15u) * 17u);
}

void DecodeBC3Block(const uint8_t* block, uint32_t out[16]) {
    Bc1Colors(block + 8, out, false);
    const uint32_t a0 = block[0], a1 = block[1];
    uint32_t a[8] = {a0, a1};
    if (a0 > a1) for (uint32_t i = 1; i < 7; ++i) a[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    else { for (uint32_t i = 1; i < 5; ++i) a[i + 1] = ((5 - i) * a0 + i * a1) / 5; a[6] = 0; a[7] = 255; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= (uint64_t)block[2 + i] << (8 * i);
    for (int i = 0; i < 16; ++i) SetAlpha(out[i], a[(bits >> (3 * i)) & 7]);
}

void DecodeBC7Block(const uint8_t* block, uint32_t out[16]) {
    int mode = 0;
    while (mode < 8 && !((block[0] >> mode) & 1)) ++mode;
    if (mode == 8) { for (int i = 0; i < 16; ++i) out[i] = 0; return; }   // reserved: transparent black
    const Bc7Mode& m = kModes[mode];
    Bits br{block, (unsigned)mode + 1};
    const uint32_t partition = br.Get(m.partBits);
    const uint32_t rotation = br.Get(m.rotBits);
    const uint32_t indexSel = br.Get(m.selBits);
    const int ne = m.subsets * 2;
    uint32_t ep[6][4] = {};
    for (int c = 0; c < 3; ++c)
        for (int e = 0; e < ne; ++e) ep[e][c] = br.Get(m.colorBits);
    if (m.alphaBits)
        for (int e = 0; e < ne; ++e) ep[e][3] = br.Get(m.alphaBits);
    unsigned cBits = m.colorBits, aBits = m.alphaBits;
    if (m.pBits == 1) {
        for (int e = 0; e < ne; ++e) {
            const uint32_t p = br.Get(1);
            for (int c = 0; c < 4; ++c) ep[e][c] = (ep[e][c] << 1) | p;
        }
        ++cBits; if (aBits) ++aBits;
    } else if (m.pBits == 2) {
        for (int s = 0; s < m.subsets; ++s) {
            const uint32_t p = br.Get(1);
            for (int e = 2 * s; e < 2 * s + 2; ++e)
                for (int c = 0; c < 3; ++c) ep[e][c] = (ep[e][c] << 1) | p;
        }
        ++cBits;
    }
    uint8_t col[6][4];
    for (int e = 0; e < ne; ++e) {
        for (int c = 0; c < 3; ++c) col[e][c] = Expand(ep[e][c], cBits);
        col[e][3] = aBits ? Expand(ep[e][3], aBits) : 255;
    }
    auto subsetOf = [&](int i) -> int {
        if (m.subsets == 2) return (kPart2[partition] >> i) & 1;
        if (m.subsets == 3) return (int)((kPart3[partition] >> (2 * i)) & 3u);
        return 0;
    };
    auto anchor = [&](int i) {
        if (i == 0) return true;
        if (m.subsets == 2) return i == kAnchor2[partition];
        if (m.subsets == 3) return i == kAnchor3a[partition] || i == kAnchor3b[partition];
        return false;
    };
    uint8_t idx[16], idx2[16] = {};
    for (int i = 0; i < 16; ++i) idx[i] = (uint8_t)br.Get(m.idxBits - (anchor(i) ? 1u : 0u));
    if (m.idx2Bits)
        for (int i = 0; i < 16; ++i) idx2[i] = (uint8_t)br.Get(m.idx2Bits - (i == 0 ? 1u : 0u));
    for (int i = 0; i < 16; ++i) {
        const int s = subsetOf(i);
        const uint8_t* e0 = col[2 * s];
        const uint8_t* e1 = col[2 * s + 1];
        uint8_t wc, wa;
        if (!m.idx2Bits) wc = wa = Weights(m.idxBits)[idx[i]];
        else if (indexSel) { wc = Weights(m.idx2Bits)[idx2[i]]; wa = Weights(m.idxBits)[idx[i]]; }
        else { wc = Weights(m.idxBits)[idx[i]]; wa = Weights(m.idx2Bits)[idx2[i]]; }
        uint8_t px[4] = {Lerp64(e0[0], e1[0], wc), Lerp64(e0[1], e1[1], wc), Lerp64(e0[2], e1[2], wc),
                         Lerp64(e0[3], e1[3], wa)};
        if (rotation) { const uint8_t t = px[3]; px[3] = px[rotation - 1]; px[rotation - 1] = t; }
        out[i] = Pack(px[0], px[1], px[2], px[3]);
    }
}

bool DdsParse(const uint8_t* p, size_t n, DdsInfo& out) {
    out = DdsInfo{};
    if (!p || n < 128 || U32(p) != FourCC('D', 'D', 'S', ' ') || U32(p + 4) != 124) return false;
    out.height = U32(p + 12); out.width = U32(p + 16);
    out.mips = U32(p + 28); if (out.mips == 0) out.mips = 1;
    const uint32_t pfFlags = U32(p + 80);
    out.dataOffset = 128;
    if (pfFlags & 0x4) {   // DDPF_FOURCC
        out.fourcc = U32(p + 84);
        if (out.fourcc == FourCC('D', 'X', '1', '0')) {
            if (n < 148) return false;
            out.dxgi = U32(p + 128);
            out.dataOffset = 148;
            switch (out.dxgi) {
            case 70: case 71: case 72: out.kind = DdsKind::BC1; break;
            case 73: case 74: case 75: out.kind = DdsKind::BC2; break;
            case 76: case 77: case 78: out.kind = DdsKind::BC3; break;
            case 97: case 98: case 99: out.kind = DdsKind::BC7; break;
            case 27: case 28: case 29: out.kind = DdsKind::RGBA8; break;
            case 87: case 88: case 90: case 91: case 92: case 93: out.kind = DdsKind::BGRA8; break;
            default: break;
            }
        } else if (out.fourcc == FourCC('D', 'X', 'T', '1')) out.kind = DdsKind::BC1;
        else if (out.fourcc == FourCC('D', 'X', 'T', '2') || out.fourcc == FourCC('D', 'X', 'T', '3')) out.kind = DdsKind::BC2;
        else if (out.fourcc == FourCC('D', 'X', 'T', '4') || out.fourcc == FourCC('D', 'X', 'T', '5')) out.kind = DdsKind::BC3;
    } else if ((pfFlags & 0x40) && U32(p + 88) == 32) {   // DDPF_RGB, 32 bpp
        const uint32_t rMask = U32(p + 92);
        if (rMask == 0x000000FFu) out.kind = DdsKind::RGBA8;
        else if (rMask == 0x00FF0000u) out.kind = DdsKind::BGRA8;
    }
    if (out.kind == DdsKind::Unknown || out.width == 0 || out.height == 0 || out.width > 8192 || out.height > 8192)
        return false;
    const size_t bw = (out.width + 3) / 4, bh = (out.height + 3) / 4;
    switch (out.kind) {
    case DdsKind::BC1: out.mip0Size = bw * bh * 8; break;
    case DdsKind::BC2: case DdsKind::BC3: case DdsKind::BC7: out.mip0Size = bw * bh * 16; break;
    default: out.mip0Size = (size_t)out.width * out.height * 4; break;
    }
    return out.dataOffset + out.mip0Size <= n;
}

bool DdsDecode(const uint8_t* p, size_t n, std::vector<uint32_t>& rgba, uint32_t& w, uint32_t& h) {
    DdsInfo d;
    if (!DdsParse(p, n, d)) return false;
    w = d.width; h = d.height;
    rgba.assign((size_t)w * h, 0);
    const uint8_t* data = p + d.dataOffset;
    if (d.kind == DdsKind::RGBA8 || d.kind == DdsKind::BGRA8) {
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            const uint32_t v = U32(data + 4 * i);
            rgba[i] = d.kind == DdsKind::RGBA8 ? v : (v & 0xFF00FF00u) | ((v >> 16) & 0xFFu) | ((v & 0xFFu) << 16);
        }
        return true;
    }
    const size_t blockBytes = d.kind == DdsKind::BC1 ? 8 : 16;
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    uint32_t px[16];
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = data + ((size_t)by * bw + bx) * blockBytes;
            switch (d.kind) {
            case DdsKind::BC1: DecodeBC1Block(blk, px, true); break;
            case DdsKind::BC2: DecodeBC2Block(blk, px); break;
            case DdsKind::BC3: DecodeBC3Block(blk, px); break;
            default: DecodeBC7Block(blk, px); break;
            }
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t gx = bx * 4 + x, gy = by * 4 + y;
                    if (gx < w && gy < h) rgba[(size_t)gy * w + gx] = px[y * 4 + x];
                }
        }
    return true;
}

// TPF (PC): "TPF\0", u32 data size, u32 texture count, u8 platform (0 = PC), u8, u8 name encoding
// (1 = UTF-16LE, else single-byte), u8; then per texture u32 data offset, u32 data size, u8 format,
// u8 type, u8 mip count, u8 flags, u32 name offset, u32 "float struct follows" (0/1; when 1: u32,
// u32 length, length bytes). PC texture data is a complete DDS file.
bool TpfTextures(const uint8_t* p, size_t n, std::vector<TpfTexture>& out) {
    out.clear();
    if (!p || n < 16 || U32(p) != FourCC('T', 'P', 'F', '\0')) return false;
    const uint32_t count = U32(p + 8);
    const uint8_t platform = p[12], encoding = p[14];
    if (platform != 0 || count == 0 || count > 4096) return true;
    size_t off = 16;
    for (uint32_t i = 0; i < count; ++i) {
        if (off + 20 > n) break;
        const uint32_t dataOff = U32(p + off), dataLen = U32(p + off + 4);
        const uint8_t flags = p[off + 11];
        const uint32_t nameOff = U32(p + off + 12), floatFlag = U32(p + off + 16);
        off += 20;
        if (floatFlag == 1) {
            if (off + 8 > n) break;
            const uint32_t len = U32(p + off + 4);
            if (len > n - off - 8) break;
            off += 8 + len;
        } else if (floatFlag != 0) break;   // not the layout this walk knows: stop rather than misread
        TpfTexture t;
        if (nameOff < n) {
            if (encoding == 1) {
                for (size_t k = nameOff; k + 1 < n && t.name.size() < 128; k += 2) {
                    const uint16_t ch = U16(p + k);
                    if (!ch) break;
                    t.name.push_back(ch < 0x80 ? (char)ch : '?');
                }
            } else {
                for (size_t k = nameOff; k < n && p[k] && t.name.size() < 128; ++k) t.name.push_back((char)p[k]);
            }
        }
        // Flags 2/3 mark DCX-compressed texture data; never seen on the menu icons, so skipped.
        if ((flags & 2) || dataLen < 128 || dataOff > (uint32_t)0x7FFFFFFF - dataLen) continue;
        t.dataEnd = (size_t)dataOff + dataLen;
        if (t.dataEnd <= n && U32(p + dataOff) == FourCC('D', 'D', 'S', ' ')) { t.dds = p + dataOff; t.ddsLen = dataLen; }
        out.push_back(std::move(t));
    }
    return true;
}

void ShrinkRgba(const std::vector<uint32_t>& src, uint32_t w, uint32_t h, uint32_t maxSide,
                std::vector<uint32_t>& out, uint32_t& ow, uint32_t& oh) {
    uint32_t f = 1;
    while (w / f > maxSide || h / f > maxSide) f *= 2;
    ow = std::max(1u, w / f); oh = std::max(1u, h / f);
    if (f == 1) { out = src; return; }
    out.assign((size_t)ow * oh, 0);
    for (uint32_t y = 0; y < oh; ++y)
        for (uint32_t x = 0; x < ow; ++x) {
            uint64_t r = 0, g = 0, b = 0, a = 0;
            for (uint32_t dy = 0; dy < f; ++dy)
                for (uint32_t dx = 0; dx < f; ++dx) {
                    const uint32_t v = src[(size_t)(y * f + dy) * w + x * f + dx];
                    const uint32_t va = v >> 24;
                    r += (v & 255u) * va; g += ((v >> 8) & 255u) * va; b += ((v >> 16) & 255u) * va; a += va;
                }
            if (!a) continue;
            out[(size_t)y * ow + x] = (uint32_t)((r + a / 2) / a) | ((uint32_t)((g + a / 2) / a) << 8) |
                                      ((uint32_t)((b + a / 2) / a) << 16) | ((uint32_t)((a + f * f / 2) / (f * f)) << 24);
        }
}

std::vector<uint8_t> MakeRgbaDds(const std::vector<uint32_t>& px, uint32_t w, uint32_t h) {
    std::vector<uint8_t> d(128, 0);
    auto put = [&](size_t at, uint32_t v) { std::memcpy(d.data() + at, &v, 4); };
    std::memcpy(d.data(), "DDS ", 4);
    put(4, 124); put(8, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000); put(12, h); put(16, w); put(20, w * 4); put(28, 1);
    put(76, 32); put(80, 0x40 | 0x1); put(88, 32);                                   // DDPF_RGB | DDPF_ALPHAPIXELS
    put(92, 0x000000FFu); put(96, 0x0000FF00u); put(100, 0x00FF0000u); put(104, 0xFF000000u);
    put(108, 0x1000);
    d.resize(128 + px.size() * 4);
    std::memcpy(d.data() + 128, px.data(), px.size() * 4);
    return d;
}

int MenuIconId(const std::string& name) {
    static const char kPrefix[] = "MENU_Knowledge_";
    const size_t k = sizeof(kPrefix) - 1;
    if (name.size() <= k || name.size() > k + 6 || name.compare(0, k, kPrefix) != 0) return -1;
    int id = 0;
    for (size_t i = k; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return -1;
        id = id * 10 + (name[i] - '0');
    }
    return id;
}

} // namespace ervr
