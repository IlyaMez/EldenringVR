#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace ervr {
struct WeaponCatalogEntry {
    uint32_t id = 0;
    uint16_t type = 0, icon = 0;
    float weight = 0;
    std::string name;
    std::vector<uint32_t> variants; // unreinforced affinity rows, including the base row
};
struct WeaponCatalog {
    std::vector<WeaponCatalogEntry> weapons;
    std::string language;
    uint32_t Canonical(uint32_t equippedId) const;
};
const char* WeaponTypeName(uint16_t type);
// Pure, bounded container readers used by the catalog and tests. No game memory accesses.
struct BinderEntry {
    int32_t id = -1;
    std::string name;
    size_t offset = 0, size = 0;
    bool compressed = false;
};
bool ReadWeaponBinder(const std::vector<uint8_t>& bytes, std::vector<BinderEntry>& entries, std::string& why);
bool ReadWeaponNames(const std::vector<uint8_t>& bytes, std::map<uint32_t, std::string>& names, std::string& why);
bool BuildWeaponCatalog(const std::vector<uint8_t>& param, const std::map<uint32_t, std::string>& names,
                        WeaponCatalog& out, std::string& why);
// Read-only. Run on a background thread. Names/icons come from this installation, including DLC.
bool LoadWeaponCatalog(const std::wstring& gameDirectory, const std::string& language, WeaponCatalog& out,
                       std::string& why);
} // namespace ervr
