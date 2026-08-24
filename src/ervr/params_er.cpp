// SoloParamRepository access. Layouts from fromsoftware-rs (MIT/Apache):
//   SoloParamRepository { FD4ResRep res_rep /*0x78*/; u32 unk78; SoloParamHolder holders[194] @0x80 }
//   SoloParamHolder     { u32 count; ParamResCap* res_caps[8] @+8 }            stride 0x48
//   ParamResCap         { FD4ResCap /*0x78*/; u32 type; FD4ParamResCap* @0x80 }
//   FD4ParamResCap      { FD4ResCap /*0x78*/; u64 size; ParamFile* data @0x80 }
//   ParamFile: SoulsFormats PARAM header (0x30, +0x10 extended when 64-bit offsets), then
//   row descriptors {u32 id, u32 pad, u64 dataOff, u64 nameOff} (0x18) - data at file+dataOff.
// The static pointer's AOB is the one shared by fromsoftware-rs / EROffsets (the immediate
// after `BA` is a version-dependent count, hence the wildcard). The result is never trusted
// by shape alone: it is validated by reading holder 0's struct name back as
// "EQUIP_PARAM_WEAPON_ST", trying one and two levels of indirection.
#include "params_er.h"
#include "game_memory.h"
#include "log.h"
#include <windows.h>
#include <atomic>
#include <cstring>

namespace ervr {
namespace {

constexpr int    kHolderCount   = 194;
constexpr size_t Repo_Holders   = 0x80;
constexpr size_t Holder_Stride  = 0x48;
constexpr size_t Holder_ResCap0 = 0x08;
constexpr size_t ResCap_FD4     = 0x80;   // ParamResCap -> FD4ParamResCap*
constexpr size_t FD4_Data       = 0x80;   // FD4ParamResCap -> ParamFile*
constexpr size_t Hdr_RowCount   = 0x0A;   // u16
constexpr size_t Hdr_NameInline = 0x0C;   // char[0x20] when !(fmt2d & 0x80)
constexpr size_t Hdr_NameOffset = 0x10;   // u32 offset when (fmt2d & 0x80)
constexpr size_t Hdr_Fmt2D      = 0x2D;
constexpr size_t Hdr_Size       = 0x30;

uint8_t** g_static = nullptr;              // address of the SoloParamRepository* global
std::atomic<uint8_t*> g_repo{nullptr};     // validated repository
bool g_scanned = false, g_loggedFail = false;

uint8_t* Deref(const void* p) { uint8_t* v = nullptr; return SafeRead(p, &v, sizeof(v)) ? v : nullptr; }

struct FileInfo {
    uint8_t* file = nullptr;
    uint32_t rows = 0;
    uint8_t* descs = nullptr;
    size_t   descStride = 0;
    bool     is64 = false;
};

bool ReadFileInfo(uint8_t* repo, int index, FileInfo& fi) {
    if (!repo || index < 0 || index >= kHolderCount) return false;
    uint8_t* holder = repo + Repo_Holders + (size_t)index * Holder_Stride;
    uint32_t count = 0;
    if (!SafeRead(holder, &count, 4) || count == 0 || count > 8) return false;
    uint8_t* rc = Deref(holder + Holder_ResCap0); if (!rc) return false;
    uint8_t* prc = Deref(rc + ResCap_FD4);         if (!prc) return false;
    uint8_t* file = Deref(prc + FD4_Data);         if (!file) return false;
    uint16_t rows16 = 0; uint8_t fmt = 0;
    if (!SafeRead(file + Hdr_RowCount, &rows16, 2) || !SafeRead(file + Hdr_Fmt2D, &fmt, 1)) return false;
    const bool is64 = (fmt & 0x04) != 0;
    const bool ext = is64 || ((fmt & 0x01) && (fmt & 0x02));
    fi.file = file; fi.rows = rows16; fi.is64 = is64;
    fi.descs = file + Hdr_Size + (ext ? 0x10 : 0);
    fi.descStride = is64 ? 0x18 : 0x0C;
    // The runtime keeps a u32 row count 0xC bytes before the file; the header's u16 wraps past
    // 65535 rows. Only believe the metadata when it agrees with the header modulo 2^16.
    uint32_t mrows = 0;
    if (SafeRead(file - 0x0C, &mrows, 4) && mrows >= rows16 && mrows < 400000 && ((mrows - rows16) & 0xFFFF) == 0) fi.rows = mrows;
    return true;
}

bool StructName(const FileInfo& fi, char* out, size_t cap) {
    uint8_t fmt = 0; if (!SafeRead(fi.file + Hdr_Fmt2D, &fmt, 1)) return false;
    const uint8_t* src = fi.file + Hdr_NameInline;
    if (fmt & 0x80) { uint32_t off = 0; if (!SafeRead(fi.file + Hdr_NameOffset, &off, 4) || !off) return false; src = fi.file + off; }
    size_t n = 0;
    for (; n + 1 < cap; ++n) { char ch = 0; if (!SafeRead(src + n, &ch, 1) || !ch) break; if (ch < 0x20 || ch > 0x7E) return false; out[n] = ch; }
    out[n] = 0;
    return n > 0;
}

bool Validate(uint8_t* repo) {
    FileInfo fi; char name[48];
    return ReadFileInfo(repo, PI_EquipParamWeapon, fi) && StructName(fi, name, sizeof(name)) && strcmp(name, "EQUIP_PARAM_WEAPON_ST") == 0;
}

} // namespace

void ParamsInit() {
    if (g_scanned) return;
    g_scanned = true;
    uint8_t* hit = AobScan("48 8B 0D ?? ?? ?? ?? 48 85 C9 0F 84 ?? ?? ?? ?? 45 33 C0 BA ?? 00 00 00 E8");
    if (!hit) hit = AobScan("48 8B 0D ?? ?? ?? ?? 48 85 C9 0F 84 ?? ?? ?? ?? 45 33 C0 BA ?? 00 00 00");
    if (!hit) { LOGW("[params] SoloParamRepository AOB not found; param-driven combat scaling unavailable"); return; }
    g_static = (uint8_t**)ResolveRipRelative(hit, 3, 7);
    LOGI("[params] SoloParamRepository* @ %p (aob hit %p)", (void*)g_static, (void*)hit);
}

bool ParamsReady() {
    if (g_repo.load(std::memory_order_acquire)) return true;
    if (!g_static) return false;
    uint8_t* r1 = Deref(g_static);
    if (!r1) return false;   // params not loaded yet
    uint8_t* found = nullptr;
    if (Validate(r1)) found = r1;
    else if (uint8_t* r2 = Deref(r1); r2 && Validate(r2)) found = r2;
    if (!found) {
        if (!g_loggedFail) { g_loggedFail = true; LOGW("[params] repository @ %p failed validation (holder 0 is not EQUIP_PARAM_WEAPON_ST)", (void*)r1); }
        return false;
    }
    g_repo.store(found, std::memory_order_release);
    const int interesting[] = {PI_EquipParamWeapon, PI_AtkParam_Pc, PI_BehaviorParam_PC, PI_SpEffectParam};
    for (int idx : interesting) {
        uint32_t sz = 0; FileInfo fi; char name[48] = "?";
        if (ReadFileInfo(found, idx, fi)) {
            StructName(fi, name, sizeof(name));
            if (fi.rows >= 2) {
                uint64_t o0 = 0, o1 = 0;
                if (fi.is64) { SafeRead(fi.descs + 8, &o0, 8); SafeRead(fi.descs + fi.descStride + 8, &o1, 8); }
                else { uint32_t a = 0, b = 0; SafeRead(fi.descs + 4, &a, 4); SafeRead(fi.descs + fi.descStride + 4, &b, 4); o0 = a; o1 = b; }
                sz = (uint32_t)(o1 > o0 ? o1 - o0 : o0 - o1);
            }
        }
        LOGI("[params] holder %d: %s rows %u rowSize 0x%X %s", idx, name, fi.rows, sz, fi.is64 ? "(64-bit offsets)" : "(32-bit offsets)");
    }
    return true;
}

uint8_t* ParamsRow(int index, uint32_t rowId, uint32_t* rowSize) {
    uint8_t* repo = g_repo.load(std::memory_order_acquire);
    FileInfo fi;
    if (!repo || !ReadFileInfo(repo, index, fi) || fi.rows == 0) return nullptr;
    // Linear scan: a few lookups per strike over a few thousand descriptors is nothing, and
    // it needs no assumption about the runtime lookup table living after the file.
    uint64_t prevOff = 0; bool havePrev = false; uint32_t stride = 0;
    for (uint32_t i = 0; i < fi.rows; ++i) {
        const uint8_t* d = fi.descs + (size_t)i * fi.descStride;
        uint32_t id = 0; if (!SafeRead(d, &id, 4)) return nullptr;
        uint64_t off = 0;
        if (fi.is64) { if (!SafeRead(d + 8, &off, 8)) return nullptr; }
        else { uint32_t o = 0; if (!SafeRead(d + 4, &o, 4)) return nullptr; off = o; }
        if (i < 2) { if (havePrev) stride = (uint32_t)(off > prevOff ? off - prevOff : prevOff - off); prevOff = off; havePrev = true; }
        if (id == rowId) {
            if (rowSize) {
                if (!stride && i + 1 < fi.rows) {
                    const uint8_t* n = fi.descs + (size_t)(i + 1) * fi.descStride; uint64_t noff = 0;
                    if (fi.is64) SafeRead(n + 8, &noff, 8); else { uint32_t o = 0; SafeRead(n + 4, &o, 4); noff = o; }
                    stride = (uint32_t)(noff > off ? noff - off : off - noff);
                }
                *rowSize = stride;
            }
            return off ? fi.file + off : nullptr;
        }
    }
    return nullptr;
}

uint8_t* ParamsRowAt(int index, uint32_t position, uint32_t* rowId) {
    uint8_t* repo = g_repo.load(std::memory_order_acquire);
    FileInfo fi;
    if (!repo || !ReadFileInfo(repo, index, fi) || position >= fi.rows) return nullptr;
    const uint8_t* d = fi.descs + (size_t)position * fi.descStride;
    uint32_t id = 0; uint64_t off = 0;
    if (!SafeRead(d, &id, 4)) return nullptr;
    if (fi.is64) { if (!SafeRead(d + 8, &off, 8)) return nullptr; }
    else { uint32_t o = 0; if (!SafeRead(d + 4, &o, 4)) return nullptr; off = o; }
    if (rowId) *rowId = id;
    return off ? fi.file + off : nullptr;
}

const char* ParamsStructName(int index) {
    static thread_local char name[48];
    name[0] = 0;
    uint8_t* repo = g_repo.load(std::memory_order_acquire);
    FileInfo fi;
    if (repo && ReadFileInfo(repo, index, fi)) StructName(fi, name, sizeof(name));
    return name;
}

uint32_t ParamsRowCount(int index) {
    uint8_t* repo = g_repo.load(std::memory_order_acquire);
    FileInfo fi;
    return repo && ReadFileInfo(repo, index, fi) ? fi.rows : 0;
}
}
