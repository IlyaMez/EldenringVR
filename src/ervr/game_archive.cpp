#include "game_archive.h"
#include "log.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <zstd.h>
#include <zlib.h>

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
std::wstring g_directory;
HMODULE g_oodle = nullptr;
bool g_oodleOwned = false;

std::wstring GameDir() {
    if (!g_directory.empty()) return g_directory;
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}
void SelectDirectory(const std::wstring& dir) {
    if (dir == g_directory) return;
    for (auto& a : g_archives) { if (a.bdt != INVALID_HANDLE_VALUE) CloseHandle(a.bdt); a = Archive{}; }
    g_icons.clear(); g_iconsTried = false; g_iconsWhy.clear(); g_iconBdt = INVALID_HANDLE_VALUE;
    if (g_oodleOwned && g_oodle) FreeLibrary(g_oodle);
    g_oodle = nullptr; g_oodleOwned = false; g_directory = dir;
}
bool ReadWhole(const std::wstring& path, std::vector<uint8_t>& out);
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
    std::vector<uint8_t> executable;
    if (!ReadWhole(GameDir() + L"\\eldenring.exe", executable)) return keys;
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!At(executable, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        !At(executable, (size_t)dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.NumberOfSections > 96) return keys;
    const size_t secTable = (size_t)dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    for (int s = 0; s < nt.FileHeader.NumberOfSections; ++s) {
        IMAGE_SECTION_HEADER sec{};
        if (!At(executable, secTable + s * sizeof(sec), sec) || std::memcmp(sec.Name, ".rdata", 6) != 0) continue;
        if (sec.PointerToRawData > executable.size() || sec.SizeOfRawData > executable.size() - sec.PointerToRawData) return {};
        std::vector<uint8_t> img(executable.begin() + sec.PointerToRawData, executable.begin() + sec.PointerToRawData + sec.SizeOfRawData);
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
    if (e.padded <= 0 || e.padded > (128 << 20) || e.offset < 0) return false;
    out.resize((size_t)e.padded);
    if (!ReadAt(a.bdt, e.offset, out.data(), out.size())) return false;
    if (e.aesAt) {
        uint8_t key[16]; int32_t ranges = 0;
        if (e.aesAt > a.bhd.size() || a.bhd.size() - (size_t)e.aesAt < 20) return false;
        std::memcpy(key, a.bhd.data() + e.aesAt, 16); At(a.bhd, (size_t)e.aesAt + 16, ranges);
        if (ranges < 0 || ranges > 64) return false;
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

bool Decompress(const std::vector<uint8_t>& dcx, std::vector<uint8_t>& out, std::string& why) {
    auto fail = [&](const char* text) { why = text; out.clear(); return false; };
    if (dcx.size() < 4 || dcx.size() > (128u << 20)) return fail("invalid container size");
    if (std::memcmp(dcx.data(), "DCX", 4) != 0) { out = dcx; return true; }
    auto tag = [&](const char* text) -> size_t {
        for (size_t i = 0; i + 12 <= std::min<size_t>(dcx.size(), 0x80); i += 4)
            if (!std::memcmp(dcx.data() + i, text, 4)) return i;
        return SIZE_MAX;
    };
    const size_t dcs = tag("DCS"), dcp = tag("DCP"), dca = tag("DCA");
    if (dcs == SIZE_MAX || dcp == SIZE_MAX || dca == SIZE_MAX) return fail("invalid DCX header");
    const uint32_t raw = BE32(dcx.data() + dcs + 4), compressed = BE32(dcx.data() + dcs + 8), head = BE32(dcx.data() + dca + 4);
    if (!raw || raw > (128u << 20) || head < 8 || head > dcx.size() - dca || compressed > dcx.size() - dca - head) return fail("invalid DCX sizes");
    const uint8_t* payload = dcx.data() + dca + head;
    out.resize(raw);
    if (!std::memcmp(dcx.data() + dcp + 4, "ZSTD", 4)) {
        const size_t got = ZSTD_decompress(out.data(), out.size(), payload, compressed);
        return (!ZSTD_isError(got) && got == raw) || fail("Zstandard decompression failed");
    }
    if (!std::memcmp(dcx.data() + dcp + 4, "DFLT", 4)) {
        uLongf got = raw;
        return (uncompress(out.data(), &got, payload, compressed) == Z_OK && got == raw) || fail("zlib decompression failed");
    }
    if (std::memcmp(dcx.data() + dcp + 4, "KRAK", 4)) return fail("unsupported DCX codec");
    if (!g_oodle) {
        if (g_directory.empty()) g_oodle = GetModuleHandleW(L"oo2core_6_win64.dll");
        if (!g_oodle) {
            g_oodle = LoadLibraryExW((GameDir() + L"\\oo2core_6_win64.dll").c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            g_oodleOwned = g_oodle != nullptr;
        }
    }
    const auto decompress = g_oodle ? reinterpret_cast<FnDecompress>(GetProcAddress(g_oodle, "OodleLZ_Decompress")) : nullptr;
    if (!decompress) return fail("the game's oo2core_6_win64.dll could not be loaded");
    return decompress(payload, compressed, out.data(), raw, 1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3) == raw || fail("Oodle decompression failed");
}

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

bool ArchiveReadItemIcon(int iconId, std::vector<uint8_t>& tpf, std::string* why, const std::wstring& gameDirectory) {
    std::lock_guard<std::mutex> g(g_mtx);
    SelectDirectory(gameDirectory);
    auto fail = [&](const std::string& w) { if (why) *why = w; return false; };
    if (!OpenIcons()) return fail(g_iconsWhy);
    auto it = g_icons.find(iconId);
    if (it == g_icons.end()) return fail("not in menu/hi/00_solo");
    if (it->second.size <= 0x50 || it->second.size > (16 << 20)) return fail("bad entry size");
    std::vector<uint8_t> dcx((size_t)it->second.size);
    if (!ReadAt(g_iconBdt, g_tpfbdtOffset + it->second.offset, dcx.data(), dcx.size())) return fail("read failed");
    std::string error;
    return Decompress(dcx, tpf, error) || fail(error);
}

bool ArchiveDecompress(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& out, std::string& why, const std::wstring& gameDirectory) {
    std::lock_guard<std::mutex> g(g_mtx); SelectDirectory(gameDirectory);
    return Decompress(bytes, out, why);
}

bool ArchiveReadFile(const char* path, std::vector<uint8_t>& bytes, std::string& why, const std::wstring& gameDirectory) {
    std::lock_guard<std::mutex> g(g_mtx); SelectDirectory(gameDirectory);
    const std::string name(path ? path : "");
    if (name.empty() || name.front() != '/' || name.find("..") != std::string::npos || name.find(':') != std::string::npos) { why = "invalid archive path"; return false; }
    std::vector<uint8_t> raw;
    if (ReadWhole((std::filesystem::path(GameDir()) / std::filesystem::path(name.substr(1))).wstring(), raw)) return Decompress(raw, bytes, why);
    const auto keys = FindKeys();
    if (keys.empty()) { why = "eldenring.exe has no readable archive keys"; return false; }
    const uint64_t hash = ArchivePathHash(path);
    for (int i = 4; i >= 0; --i) {
        auto& a = g_archives[i];
        if (!OpenArchive(a, kArchiveNames[i], keys)) continue;
        const auto it = a.files.find(hash);
        if (it != a.files.end()) {
            if (!ReadFileFrom(a, it->second, raw)) { why = "archive entry could not be read"; return false; }
            return Decompress(raw, bytes, why);
        }
    }
    why = name + " was not found in the installed game"; return false;
}

bool ArchiveReadRegulation(std::vector<uint8_t>& out, std::string& why, const std::wstring& gameDirectory) {
    std::lock_guard<std::mutex> g(g_mtx); SelectDirectory(gameDirectory);
    std::vector<uint8_t> input;
    if (!ReadWhole(GameDir() + L"\\regulation.bin", input) || input.size() < 32 || input.size() % 16) { why = "regulation.bin is missing or invalid"; return false; }
    // ER regulation AES-256-CBC format/key: SoulsFormatsNEXT RegulationDecryptor (see THIRD_PARTY.md).
    const uint8_t secret[32] = {0x99,0xBF,0xFC,0x36,0x6A,0x6B,0xC8,0xC6,0xF5,0x82,0x7D,0x09,0x36,0x02,0xD6,0x76,0xC4,0x28,0x92,0xA0,0x1C,0x20,0x7F,0xB0,0x24,0xD3,0xAF,0x4E,0x49,0x3F,0xEF,0x99};
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0)) &&
              BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &key, nullptr, 0, (PUCHAR)secret, sizeof(secret), 0));
    std::vector<uint8_t> raw(input.size()); ULONG got = 0; uint8_t iv[16]; std::memcpy(iv, input.data(), 16);
    ok = ok && BCRYPT_SUCCESS(BCryptDecrypt(key, input.data()+16, (ULONG)input.size()-16, nullptr, iv, 16, raw.data(), (ULONG)raw.size(), &got, 0));
    if (key) BCryptDestroyKey(key); if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) { why = "regulation.bin decryption failed"; return false; }
    raw.resize(got); return Decompress(raw, out, why);
}

}
