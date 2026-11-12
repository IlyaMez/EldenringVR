#include "rtti.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>
#include <cstring>

namespace ervr {
namespace {

struct Section { uint8_t* start; size_t size; bool exec; };

std::vector<Section> Sections() {
    std::vector<Section> out;
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & (IMAGE_SCN_MEM_READ))) continue;
        if (sec->Characteristics & IMAGE_SCN_MEM_DISCARDABLE) continue;
        out.push_back({ base + sec->VirtualAddress, sec->Misc.VirtualSize, (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 });
    }
    return out;
}

// "CS::NoUpdateInterface" -> ".?AVNoUpdateInterface@CS@@"
std::string MangledName(const char* demangled) {
    std::vector<std::string> parts;
    std::string s(demangled), cur;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s.compare(i, 2, "::") == 0) { parts.push_back(cur); cur.clear(); ++i; }
        else cur.push_back(s[i]);
    }
    parts.push_back(cur);
    std::string m = ".?AV";
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) m += *it + "@";
    m += "@";
    return m;
}

const uint8_t* FindBytes(const uint8_t* hay, size_t n, const uint8_t* needle, size_t m) {
    if (m == 0 || n < m) return nullptr;
    for (size_t i = 0; i + m <= n; ++i) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, m) == 0) return hay + i;
    }
    return nullptr;
}

} // namespace

void** FindVTable(const char* className) {
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    std::string mangled = MangledName(className);
    auto secs = Sections();

    // 1) TypeDescriptor: { void* vft; void* spare; char name[]; } -> find name, descriptor = name - 0x10.
    const uint8_t* td = nullptr;
    for (auto& s : secs) {
        if (s.exec) continue;
        const uint8_t* p = s.start; size_t left = s.size;
        while (left) {
            const uint8_t* hit = FindBytes(p, left, (const uint8_t*)mangled.c_str(), mangled.size() + 1);
            if (!hit) break;
            td = hit - 0x10; break;
        }
        if (td) break;
    }
    if (!td) { LOGW("[rtti] type descriptor for %s not found", className); return nullptr; }
    uint32_t tdRva = (uint32_t)(td - base);

    // 2) CompleteObjectLocator: { u32 sig=1; u32 offset; u32 cdOffset; u32 tdRva; u32 chdRva; u32 selfRva; }
    //    Primary vtable has offset==0.
    const uint8_t* col = nullptr;
    for (auto& s : secs) {
        if (s.exec) continue;
        for (size_t off = 0; off + 24 <= s.size; off += 4) {
            const uint32_t* w = (const uint32_t*)(s.start + off);
            if (w[3] != tdRva || w[0] != 1 || w[1] != 0) continue;
            if (w[5] != (uint32_t)((s.start + off) - base)) continue;
            col = s.start + off; break;
        }
        if (col) break;
    }
    if (!col) { LOGW("[rtti] complete object locator for %s not found", className); return nullptr; }

    // 3) vtable: the pointer-sized slot holding &COL, vtable starts right after it.
    for (auto& s : secs) {
        if (s.exec) continue;
        for (size_t off = 0; off + 8 <= s.size; off += 8) {
            if (*(const uint8_t* const*)(s.start + off) == col) {
                void** vt = (void**)(s.start + off + 8);
                LOGI("[rtti] %s vtable at %p", className, (void*)vt);
                return vt;
            }
        }
    }
    LOGW("[rtti] vtable for %s not found", className);
    return nullptr;
}

namespace {
// ".?AVCSTask@CS@@" -> "CS::CSTask"
std::string Demangle(const char* mangled) {
    std::string s(mangled);
    if (s.rfind(".?AV", 0) != 0 && s.rfind(".?AU", 0) != 0) return {};
    s = s.substr(4);
    if (s.size() >= 2 && s.compare(s.size() - 2, 2, "@@") == 0) s = s.substr(0, s.size() - 2);
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == '@') { parts.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) parts.push_back(cur);
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!out.empty()) out += "::";
        out += *it;
    }
    return out;
}
} // namespace

std::vector<std::string> FindTypeNames(const char* substr) {
    std::vector<std::string> out;
    for (auto& s : Sections()) {
        if (s.exec) continue;
        for (size_t off = 0; off + 8 < s.size; ++off) {
            const char* p = (const char*)(s.start + off);
            if (p[0] != '.' || p[1] != '?' || p[2] != 'A' || (p[3] != 'V' && p[3] != 'U')) continue;
            size_t len = 0;
            while (len < 512 && off + len < s.size && p[len]) ++len;
            if (len == 0 || len >= 512) continue;
            if (!strstr(p, substr)) { off += len; continue; }
            std::string d = Demangle(p);
            if (!d.empty()) out.push_back(d);
            off += len;
        }
    }
    return out;
}

std::string RttiNameOfVTable(void** vt) {
    // MSVC puts a pointer to the CompleteObjectLocator immediately BEFORE the vtable.
    if (!vt) return {};
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto inModule = [&](const void* p) { return p >= base && p < base + 0x10000000ull; };
    void* colp = *((void**)vt - 1);
    if (!inModule(colp)) return {};
    const uint32_t* col = (const uint32_t*)colp;
    if (col[0] != 1) return {};                       // signature: 1 = 64-bit COL
    const uint8_t* td = base + col[3];                // type descriptor RVA
    if (!inModule(td)) return {};
    const char* name = (const char*)(td + 0x10);
    for (int i = 0; i < 4; ++i) if (name[i] == 0) return {};
    return Demangle(name);
}

std::vector<void*> ModuleStaticPointers() {
    std::vector<void*> out;
    for (auto& s : Sections()) {
        if (s.exec) continue;
        for (size_t off = 0; off + 8 <= s.size; off += 8) {
            void* cand = *(void* const*)(s.start + off);
            auto v = (uintptr_t)cand;
            if (v < 0x10000 || v >= 0x7FFFFFFFFFFFull || (v & 7)) continue;
            out.push_back(cand);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// The game frees memory while the scan runs (2026-11-12: a block committed at VirtualQuery
// time was gone at the read during boot, and Seamless Co-op's crash handler ended the game on
// the fault). Its own function so the __try sits next to no C++ objects.
static bool ReadsVTable(const void* cand, void** vt) {
    __try { return *(void* const*)cand == (void*)vt; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::vector<void*> FindInstancesOfVTable(void** vt, int maxHits) {
    std::vector<void*> out;
    if (!vt) return out;
    for (auto& s : Sections()) {
        if (s.exec) continue;
        for (size_t off = 0; off + 8 <= s.size; off += 8) {
            void* cand = *(void* const*)(s.start + off);
            // Cheap filters first: a heap object pointer is aligned and in user space. This
            // rejects almost everything before the expensive VirtualQuery.
            auto v = (uintptr_t)cand;
            if (v < 0x10000 || v >= 0x7FFFFFFFFFFFull || (v & 7)) continue;
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(cand, &mbi, sizeof(mbi))) continue;
            if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
            if ((uint8_t*)cand + 8 > (uint8_t*)mbi.BaseAddress + mbi.RegionSize) continue;
            if (!ReadsVTable(cand, vt)) continue;
            out.push_back(cand);
            if ((int)out.size() >= maxHits) return out;
        }
    }
    return out;
}
}
