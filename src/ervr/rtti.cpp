#include "rtti.h"
#include "log.h"
#include <windows.h>
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
}
