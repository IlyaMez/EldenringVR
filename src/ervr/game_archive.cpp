#include "game_archive.h"
#include "game_memory.h"
#include "log.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ervr {
namespace {

struct RsaKey { std::vector<uint8_t> n, e; };   // big-endian, no sign byte
struct FileEntry { int64_t offset = 0; int32_t padded = 0, unpadded = 0; uint64_t aesAt = 0; };
struct Archive {
    bool tried = false, ok = false;
    std::string why;
    std::vector<uint8_t> bhd;              // the decrypted index (AES key records are read from it)
    std::unordered_map<uint64_t, FileEntry> files;
    HANDLE bdt = INVALID_HANDLE_VALUE;
};
struct IconEntry { int64_t offset = 0, size = 0; };
std::mutex g_mtx;
bool g_iconsTried = false;
std::string g_iconsWhy;
HANDLE g_iconBdt = INVALID_HANDLE_VALUE;   // the .bdt holding 00_solo.tpfbdt (owned by its Archive)
int64_t g_tpfbdtOffset = 0;
std::unordered_map<int, IconEntry> g_icons;
Archive g_archives[5];
const wchar_t* const kArchiveNames[5] = {L"Data0", L"Data1", L"Data2", L"Data3", L"DLC"};

std::wstring GameDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}
uint32_t BE32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
template <class T> bool At(const std::vector<uint8_t>& b, size_t off, T& out) {
    if (off > b.size() || b.size() - off < sizeof(T)) return false;
    std::memcpy(&out, b.data() + off, sizeof(T));
    return true;
}

// ---- the public keys: PKCS#1 RSAPublicKey PEM blocks in the executable's .rdata ----
bool Base64(const char* s, size_t n, std::vector<uint8_t>& out) {
    uint32_t acc = 0; int bits = 0;
    out.clear();
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
              : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (v < 0) continue;   // line breaks, '=' padding
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)(acc >> bits)); }
    }
    return !out.empty();
}
bool DerLength(const std::vector<uint8_t>& d, size_t& i, size_t& len) {
    if (i >= d.size()) return false;
    len = d[i++];
    if (len & 0x80) {
        int k = (int)(len & 0x7F); len = 0;
        if (k > 4) return false;
        while (k--) { if (i >= d.size()) return false; len = (len << 8) | d[i++]; }
    }
    return i + len <= d.size();
}
bool DerInt(const std::vector<uint8_t>& d, size_t& i, std::vector<uint8_t>& out) {
    if (i >= d.size() || d[i++] != 0x02) return false;
    size_t len = 0;
    if (!DerLength(d, i, len)) return false;
    out.assign(d.begin() + i, d.begin() + i + len); i += len;
    while (out.size() > 1 && out[0] == 0) out.erase(out.begin());
    return !out.empty();
}
std::vector<RsaKey> FindKeys() {
    std::vector<RsaKey> keys;
    const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!SafeRead(base, &dos, sizeof(dos)) || !SafeRead(base + dos.e_lfanew, &nt, sizeof(nt))) return keys;
    const uint8_t* secTable = base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    for (int s = 0; s < nt.FileHeader.NumberOfSections; ++s) {
        IMAGE_SECTION_HEADER sec{};
        if (!SafeRead(secTable + s * sizeof(sec), &sec, sizeof(sec)) || std::memcmp(sec.Name, ".rdata", 6) != 0) continue;
        std::vector<uint8_t> img(sec.Misc.VirtualSize, 0);
        for (size_t at = 0; at < img.size(); at += 1 << 20)
            SafeRead(base + sec.VirtualAddress + at, img.data() + at, std::min<size_t>(1 << 20, img.size() - at));
        static const char kBegin[] = "-----BEGIN RSA PUBLIC KEY-----", kEnd[] = "-----END RSA PUBLIC KEY-----";
        auto it = img.begin();
        while ((it = std::search(it, img.end(), kBegin, kBegin + sizeof(kBegin) - 1)) != img.end()) {
            auto body = it + (sizeof(kBegin) - 1);
            auto limit = body + std::min<ptrdiff_t>(img.end() - body, 4096);
            auto end = std::search(body, limit, kEnd, kEnd + sizeof(kEnd) - 1);
            it = body;
            if (end == limit) continue;
            std::vector<uint8_t> der; RsaKey k;
            size_t i = 0, len = 0;
            if (Base64((const char*)&*body, (size_t)(end - body), der) && der[i++] == 0x30 && DerLength(der, i, len) &&
                DerInt(der, i, k.n) && DerInt(der, i, k.e))
                keys.push_back(std::move(k));
        }
    }
    return keys;
}

// ---- RSA public operation and AES-ECB, by Windows CNG ----
struct Rsa {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE key = nullptr; size_t bytes = 0;
    ~Rsa() { if (key) BCryptDestroyKey(key); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
    bool Open(const RsaKey& k) {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0))) return false;
        std::vector<uint8_t> blob(sizeof(BCRYPT_RSAKEY_BLOB) + k.e.size() + k.n.size());
        auto* h = (BCRYPT_RSAKEY_BLOB*)blob.data();
        h->Magic = BCRYPT_RSAPUBLIC_MAGIC; h->BitLength = (ULONG)(k.n.size() * 8);
        h->cbPublicExp = (ULONG)k.e.size(); h->cbModulus = (ULONG)k.n.size(); h->cbPrime1 = h->cbPrime2 = 0;
        std::memcpy(blob.data() + sizeof(BCRYPT_RSAKEY_BLOB), k.e.data(), k.e.size());
        std::memcpy(blob.data() + sizeof(BCRYPT_RSAKEY_BLOB) + k.e.size(), k.n.data(), k.n.size());
        bytes = k.n.size();
        return BCRYPT_SUCCESS(BCryptImportKeyPair(alg, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key, blob.data(), (ULONG)blob.size(), 0));
    }
    // c^e mod n: the archive's "decryption" is the public operation, which CNG exposes as raw encryption.
    bool Public(const uint8_t* in, uint8_t* out) const {
        ULONG got = 0;
        return BCRYPT_SUCCESS(BCryptEncrypt(key, (PUCHAR)in, (ULONG)bytes, nullptr, nullptr, 0, out, (ULONG)bytes, &got, BCRYPT_PAD_NONE)) &&
               got == bytes;
    }
};
bool AesEcbDecrypt(const uint8_t key16[16], uint8_t* data, size_t len) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0)) &&
              BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &key, nullptr, 0, (PUCHAR)key16, 16, 0));
    ULONG got = 0;
    ok = ok && BCRYPT_SUCCESS(BCryptDecrypt(key, data, (ULONG)len, nullptr, nullptr, 0, data, (ULONG)len, &got, 0)) && got == len;
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool ReadAt(HANDLE f, int64_t offset, void* out, size_t len) {
    LARGE_INTEGER li; li.QuadPart = offset;
    if (!SetFilePointerEx(f, li, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ReadFile(f, out, (DWORD)len, &got, nullptr) && got == len;
}
bool ReadWhole(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < (256 << 20);
    if (ok) { out.resize((size_t)sz.QuadPart); ok = ReadAt(f, 0, out.data(), out.size()); }
    CloseHandle(f);
    return ok;
}

// ---- the BHD5 index ----
bool OpenArchive(Archive& a, const wchar_t* name, const std::vector<RsaKey>& keys) {
    if (a.tried) return a.ok;
    a.tried = true;
    const std::wstring dir = GameDir();
    std::vector<uint8_t> enc;
    if (!ReadWhole(dir + L"\\" + name + L".bhd", enc)) { a.why = "no index file"; return false; }
    for (const RsaKey& k : keys) {
        Rsa rsa;
        if (!rsa.Open(k) || enc.size() < rsa.bytes) continue;
        std::vector<uint8_t> block(rsa.bytes);
        if (!rsa.Public(enc.data(), block.data()) || std::memcmp(block.data() + 1, "BHD5", 4) != 0) continue;
        a.bhd.clear(); a.bhd.reserve(enc.size());
        for (size_t at = 0; at + rsa.bytes <= enc.size(); at += rsa.bytes) {
            if (!rsa.Public(enc.data() + at, block.data())) { a.why = "index decryption failed"; return false; }
            a.bhd.insert(a.bhd.end(), block.begin() + 1, block.end());   // the top byte is the zero pad
        }
        break;
    }
    if (a.bhd.size() < 0x18 || std::memcmp(a.bhd.data(), "BHD5", 4) != 0) { a.why = "no executable key opens the index"; return false; }
    int32_t buckets = 0, bucketsAt = 0;
    At(a.bhd, 0x10, buckets); At(a.bhd, 0x14, bucketsAt);
    for (int32_t b = 0; b < buckets; ++b) {
        int32_t count = 0, at = 0;
        if (!At(a.bhd, (size_t)bucketsAt + 8 * b, count) || !At(a.bhd, (size_t)bucketsAt + 8 * b + 4, at)) break;
        for (int32_t j = 0; j < count; ++j) {
            const size_t o = (size_t)at + 40 * (size_t)j;
            uint64_t hash = 0; FileEntry e;
            if (!At(a.bhd, o, hash) || !At(a.bhd, o + 8, e.padded) || !At(a.bhd, o + 12, e.unpadded) ||
                !At(a.bhd, o + 16, e.offset) || !At(a.bhd, o + 32, e.aesAt)) break;
            a.files[hash] = e;
        }
    }
    a.bdt = CreateFileW((dir + L"\\" + name + L".bdt").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, 0, nullptr);
    if (a.bdt == INVALID_HANDLE_VALUE) { a.why = "no data file"; return false; }
    a.ok = !a.files.empty();
    if (!a.ok) a.why = "empty index";
    return a.ok;
}
// A whole file from an archive, its AES ranges decrypted.
bool ReadFileFrom(Archive& a, const FileEntry& e, std::vector<uint8_t>& out) {
    if (e.padded <= 0 || e.padded > (64 << 20)) return false;
    out.resize((size_t)e.padded);
    if (!ReadAt(a.bdt, e.offset, out.data(), out.size())) return false;
    if (e.aesAt) {
        uint8_t key[16]; int32_t ranges = 0;
        if (e.aesAt + 20 > a.bhd.size()) return false;
        std::memcpy(key, a.bhd.data() + e.aesAt, 16); At(a.bhd, (size_t)e.aesAt + 16, ranges);
        for (int32_t r = 0; r < ranges && r < 64; ++r) {
            int64_t s = 0, t = 0;
            if (!At(a.bhd, (size_t)e.aesAt + 20 + 16 * r, s) || !At(a.bhd, (size_t)e.aesAt + 28 + 16 * r, t)) return false;
            if (s < 0 || t <= s) continue;
            if (t > (int64_t)out.size() || (t - s) % 16) return false;
            if (!AesEcbDecrypt(key, out.data() + s, (size_t)(t - s))) return false;
        }
    }
    if (e.unpadded > 0 && e.unpadded <= e.padded) out.resize((size_t)e.unpadded);
    return true;
}

// ---- menu/hi/00_solo.tpfbhd: the per-item icon binder's header (BHF4) ----
bool OpenIcons() {
    if (g_iconsTried) return !g_icons.empty();
    g_iconsTried = true;
    const std::vector<RsaKey> keys = FindKeys();
    if (keys.empty()) { g_iconsWhy = "the executable's archive keys were not found"; return false; }
    const uint64_t hbhd = ArchivePathHash("/menu/hi/00_solo.tpfbhd"), hbdt = ArchivePathHash("/menu/hi/00_solo.tpfbdt");
    for (int i = 0; i < 5; ++i) {
        Archive& a = g_archives[i];
        if (!OpenArchive(a, kArchiveNames[i], keys)) continue;
        auto ih = a.files.find(hbhd), id = a.files.find(hbdt);
        if (ih == a.files.end() || id == a.files.end()) continue;
        std::vector<uint8_t> b;
        if (!ReadFileFrom(a, ih->second, b) || b.size() < 0x40 || std::memcmp(b.data(), "BHF4", 4) != 0) { g_iconsWhy = "menu/hi/00_solo.tpfbhd did not read"; return false; }
        int32_t count = 0; int64_t hsize = 0;
        At(b, 0x0C, count); At(b, 0x20, hsize);
        const bool unicode = b[0x30] != 0;
        if (hsize != 36 || !unicode) { g_iconsWhy = "menu/hi/00_solo.tpfbhd has an entry layout this build does not know"; return false; }
        for (int32_t k = 0; k < count; ++k) {
            const size_t o = 0x40 + (size_t)k * 36;
            int64_t comp = 0; uint32_t offset = 0, nameAt = 0;
            if (!At(b, o + 8, comp) || !At(b, o + 24, offset) || !At(b, o + 32, nameAt)) break;
            std::string name;
            for (size_t c = nameAt; c + 1 < b.size() && name.size() < 96; c += 2) {
                const uint16_t ch = (uint16_t)(b[c] | (b[c + 1] << 8));
                if (!ch) break;
                name.push_back(ch < 0x80 ? (char)ch : '?');
            }
            static const char kKey[] = "MENU_Knowledge_";
            const size_t p = name.find(kKey);
            if (p == std::string::npos) continue;
            const int iconId = atoi(name.c_str() + p + sizeof(kKey) - 1);
            g_icons[iconId] = IconEntry{(int64_t)offset, comp};
        }
        g_iconBdt = a.bdt;
        g_tpfbdtOffset = id->second.offset;
        LOGI("[icons] reading item icons from the game's archive: %S menu/hi/00_solo, %zu icons", kArchiveNames[i], g_icons.size());
        return !g_icons.empty();
    }
    if (g_iconsWhy.empty()) g_iconsWhy = "menu/hi/00_solo is in none of the archives";
    return false;
}

using FnDecompress = intptr_t(__cdecl*)(const void*, intptr_t, void*, intptr_t, int, int, int, void*, intptr_t, void*, void*, void*, intptr_t, int);

} // namespace

uint64_t ArchivePathHash(const char* path) {
    uint64_t h = 0;
    for (const char* c = path; *c; ++c) {
        char ch = *c == '\\' ? '/' : *c;
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        h = h * 0x85 + (uint8_t)ch;
    }
    return h;
}

bool ArchiveReadItemIcon(int iconId, std::vector<uint8_t>& tpf, std::string* why) {
    std::lock_guard<std::mutex> g(g_mtx);
    auto fail = [&](const std::string& w) { if (why) *why = w; return false; };
    if (!OpenIcons()) return fail(g_iconsWhy);
    auto it = g_icons.find(iconId);
    if (it == g_icons.end()) return fail("not in menu/hi/00_solo");
    if (it->second.size <= 0x50 || it->second.size > (16 << 20)) return fail("bad entry size");
    std::vector<uint8_t> dcx((size_t)it->second.size);
    if (!ReadAt(g_iconBdt, g_tpfbdtOffset + it->second.offset, dcx.data(), dcx.size())) return fail("read failed");
    // DCX (big-endian header): "DCS" uncompressed / compressed sizes, "DCP" the codec, "DCA" the payload's start.
    auto tag = [&](const char* t) -> size_t {
        for (size_t i = 0; i + 8 <= std::min<size_t>(dcx.size(), 0x80); i += 4) if (std::memcmp(dcx.data() + i, t, 4) == 0) return i;
        return SIZE_MAX;
    };
    const size_t dcs = tag("DCS"), dcp = tag("DCP"), dca = tag("DCA");
    if (std::memcmp(dcx.data(), "DCX", 4) != 0 || dcs == SIZE_MAX || dcp == SIZE_MAX || dca == SIZE_MAX) return fail("not a DCX file");
    if (std::memcmp(dcx.data() + dcp + 4, "KRAK", 4) != 0) return fail("not Oodle-compressed");
    const uint32_t rawLen = BE32(dcx.data() + dcs + 4), compLen = BE32(dcx.data() + dcs + 8), headLen = BE32(dcx.data() + dca + 4);
    const size_t payload = dca + headLen;
    if (rawLen == 0 || rawLen > (64u << 20) || payload > dcx.size() || compLen > dcx.size() - payload) return fail("bad DCX sizes");
    static FnDecompress s_decompress = nullptr;
    if (!s_decompress) {
        HMODULE oodle = GetModuleHandleW(L"oo2core_6_win64.dll");
        if (oodle) s_decompress = (FnDecompress)GetProcAddress(oodle, "OodleLZ_Decompress");
    }
    if (!s_decompress) return fail("oo2core_6_win64.dll is not loaded");
    tpf.resize(rawLen);
    const intptr_t got = s_decompress(dcx.data() + payload, compLen, tpf.data(), rawLen, 1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
    if (got != (intptr_t)rawLen) return fail("Oodle decompression failed");
    return true;
}

}
