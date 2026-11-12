#include "weapon_catalog.h"
#include "game_archive.h"
#include "equipment_layout.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <windows.h>

namespace ervr {
namespace {
template <class T> bool Read(const std::vector<uint8_t>& b, size_t at, T& out) {
    if (at > b.size() || sizeof(T) > b.size() - at)
        return false;
    std::memcpy(&out, b.data() + at, sizeof(T));
    return true;
}
std::string Text(const std::vector<uint8_t>& b, size_t at, bool wide, size_t cap = 512) {
    if (at >= b.size() || (wide && (at & 1)))
        return {};
    if (!wide) {
        size_t end = at;
        while (end < b.size() && b[end] && end - at <= cap)
            ++end;
        return end < b.size() && end - at <= cap ? std::string((const char*)b.data() + at, end - at) : std::string();
    }
    std::wstring text;
    for (size_t i = 0; i <= cap; ++i) {
        uint16_t ch = 0;
        if (!Read(b, at + i * 2, ch))
            return {};
        if (!ch) {
            const int len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr,
                                                0, nullptr, nullptr);
            if (len <= 0)
                return {};
            std::string out(len, '\0');
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), (int)text.size(), out.data(), len, nullptr,
                                nullptr);
            return out;
        }
        text.push_back((wchar_t)ch);
    }
    return {};
}
uint8_t Reverse(uint8_t b) {
    b = (b >> 4) | (b << 4);
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
    return ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
}
bool Fail(std::string& why, const char* text) {
    why = text;
    return false;
}
} // namespace

bool ReadWeaponBinder(const std::vector<uint8_t>& b, std::vector<BinderEntry>& entries, std::string& why) {
    entries.clear();
    if (b.size() < 0x40 || std::memcmp(b.data(), "BND4", 4) || b[9])
        return Fail(why, "unsupported item binder header");
    uint32_t count = 0;
    uint64_t header = 0, stride = 0;
    Read(b, 12, count);
    Read(b, 16, header);
    Read(b, 32, stride);
    const uint8_t raw = b[0x31];
    const bool bitBig = !b[0xA];
    const uint8_t format = bitBig || ((raw & 1) && !(raw & 128)) ? raw : Reverse(raw);
    const size_t expected =
        16 + ((format & 0x20) ? 8 : 0) + ((format & 0x10) ? 8 : 4) + ((format & 2) ? 4 : 0) + ((format & 12) ? 4 : 0);
    if (header != 0x40 || count > 10000 || stride != expected || !(format & 12) || (format & 1) ||
        count > (b.size() - 0x40) / stride)
        return Fail(why, "unsupported item binder layout");
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = 0x40 + (size_t)i * stride;
        size_t pos = at + 8;
        uint64_t size = 0, offset = 0;
        uint32_t name = 0;
        Read(b, pos, size);
        pos += 8;
        if (format & 0x20)
            pos += 8;
        if (format & 0x10) {
            Read(b, pos, offset);
            pos += 8;
        } else {
            uint32_t small = 0;
            Read(b, pos, small);
            offset = small;
            pos += 4;
        }
        BinderEntry e;
        if (format & 2) {
            Read(b, pos, e.id);
            pos += 4;
        }
        Read(b, pos, name);
        if (offset > b.size() || size > b.size() - offset)
            return Fail(why, "item binder entry exceeds file");
        e.offset = (size_t)offset;
        e.size = (size_t)size;
        e.name = Text(b, name, b[0x30] != 0);
        e.compressed = ((bitBig ? b[at] : Reverse(b[at])) & 1) != 0;
        if (e.name.empty())
            return Fail(why, "invalid item binder name");
        entries.push_back(std::move(e));
    }
    return true;
}

bool ReadWeaponNames(const std::vector<uint8_t>& b, std::map<uint32_t, std::string>& names, std::string& why) {
    if (b.size() < 0x28 || b[0] || b[1] || b[2] != 2)
        return Fail(why, "unsupported weapon-name FMG");
    uint32_t groups = 0, count = 0;
    uint64_t offsets = 0;
    Read(b, 12, groups);
    Read(b, 16, count);
    Read(b, 24, offsets);
    if (groups > 100000 || count > 100000 || groups > (b.size() - 40) / 16 || offsets > b.size() ||
        count > (b.size() - offsets) / 8)
        return Fail(why, "invalid weapon-name table bounds");
    for (uint32_t i = 0; i < groups; ++i) {
        uint32_t index = 0, first = 0, last = 0;
        Read(b, 40 + i * 16, index);
        Read(b, 44 + i * 16, first);
        Read(b, 48 + i * 16, last);
        if (last < first || index >= count || last - first >= count - index)
            return Fail(why, "invalid weapon-name range");
        for (uint32_t j = 0; j <= last - first; ++j) {
            uint64_t at = 0;
            Read(b, (size_t)offsets + 8ull * (index + j), at);
            if (!at)
                continue;
            auto text = Text(b, (size_t)at, true, 256);
            if (text.empty())
                continue;
            names[first + j] = std::move(text);
        }
    }
    return true;
}

bool BuildWeaponCatalog(const std::vector<uint8_t>& b, const std::map<uint32_t, std::string>& names, WeaponCatalog& out,
                        std::string& why) {
    out.weapons.clear();
    if (b.size() < 0x58 || b[0x2C] != 0 || !(b[0x2D] & 4))
        return Fail(why, "unsupported weapon param header");
    uint16_t count16 = 0;
    Read(b, 10, count16);
    uint64_t firstData = 0;
    Read(b, 0x48, firstData);
    if (firstData < 0x58 || firstData > b.size())
        return Fail(why, "invalid weapon param offsets");
    const size_t limit = ((size_t)firstData - 0x40) / 24;
    size_t count = count16;
    while (count + 65536 <= limit)
        count += 65536;
    if (!count || count > 400000 || count > limit || firstData - 0x40 - count * 24 > 16)
        return Fail(why, "invalid weapon param row count");
    std::string structName;
    if (b[0x2D] & 128) {
        uint32_t at = 0;
        Read(b, 16, at);
        structName = Text(b, at, false);
    } else
        structName = Text(b, 12, false, 32);
    if (structName != "EQUIP_PARAM_WEAPON_ST")
        return Fail(why, "regulation weapon type does not match");
    struct Row {
        uint32_t id;
        int32_t origin, sort;
        uint16_t type, icon;
        float weight;
    };
    std::map<uint32_t, Row> rows;
    for (size_t i = 0; i < count; ++i) {
        uint32_t id = 0;
        uint64_t at = 0;
        Read(b, 0x40 + i * 24, id);
        Read(b, 0x48 + i * 24, at);
        if (id == 0 || id >= 100000000 || id % 100)
            continue;
        if (at > b.size() || b.size() - at < 0x298)
            return Fail(why, "truncated weapon row");
        Row r{};
        r.id = id;
        Read(b, (size_t)at + 0x60, r.origin);
        Read(b, (size_t)at + 8, r.sort);
        Read(b, (size_t)at + Wep_Type, r.type);
        Read(b, (size_t)at + Wep_IconId, r.icon);
        Read(b, (size_t)at + 0x10, r.weight);
        if (!std::isfinite(r.weight) || r.weight < 0 || r.weight > 1000)
            return Fail(why, "invalid weapon weight");
        rows[id] = r;
    }
    std::map<uint32_t, WeaponCatalogEntry> grouped;
    for (const auto& [id, r] : rows) {
        // Ammunition shares EquipParamWeapon, but is never the held weapon this runtime tunes.
        if (r.type == 81 || r.type == 83 || r.type == 85 || r.type == 86)
            continue;
        uint32_t base = id;
        std::set<uint32_t> visited;
        for (int depth = 0; depth < 8; ++depth) {
            if (!visited.insert(base).second)
                break;
            const auto it = rows.find(base);
            if (it == rows.end())
                break;
            const int32_t origin = it->second.origin;
            if (origin <= 0 || (uint32_t)origin == base || !rows.count((uint32_t)origin) ||
                rows.at((uint32_t)origin).type != r.type)
                break;
            base = (uint32_t)origin;
        }
        const Row& root = rows.at(base);
        const auto name = names.find(base);
        // Named, inventory-sortable rows are player catalog entries, not NPC/internal equipment.
        if (root.sort < 0 || !root.icon || name == names.end() || name->second.empty() || name->second.front() == '%' ||
            name->second == "[ERROR]")
            continue;
        auto& e = grouped[base];
        e.id = base;
        e.type = root.type;
        e.icon = root.icon;
        e.weight = root.weight;
        e.name = name->second;
        e.variants.push_back(id);
    }
    for (auto& [id, e] : grouped)
        out.weapons.push_back(std::move(e));
    if (out.weapons.empty())
        return Fail(why, "no named player weapons were found");
    return true;
}

uint32_t WeaponCatalog::Canonical(uint32_t id) const {
    id = id / 100 * 100;
    for (const auto& w : weapons)
        if (std::binary_search(w.variants.begin(), w.variants.end(), id))
            return w.id;
    return id;
}
const char* WeaponTypeName(uint16_t type) {
    switch (type) {
    case 1:
        return "Daggers";
    case 3:
        return "Straight swords";
    case 5:
        return "Greatswords";
    case 7:
        return "Colossal swords";
    case 9:
        return "Curved swords";
    case 11:
        return "Curved greatswords";
    case 13:
        return "Katanas";
    case 14:
        return "Twinblades";
    case 15:
        return "Thrusting swords";
    case 16:
        return "Heavy thrusting swords";
    case 17:
        return "Axes";
    case 19:
        return "Greataxes";
    case 21:
        return "Hammers";
    case 23:
        return "Great hammers";
    case 24:
        return "Flails";
    case 25:
        return "Spears";
    case 28:
        return "Great spears";
    case 29:
        return "Halberds";
    case 31:
        return "Reapers";
    case 35:
        return "Fists";
    case 37:
        return "Claws";
    case 39:
        return "Whips";
    case 41:
        return "Colossal weapons";
    case 50:
        return "Light bows";
    case 51:
        return "Bows";
    case 53:
        return "Greatbows";
    case 55:
        return "Crossbows";
    case 56:
        return "Ballistae";
    case 57:
        return "Glintstone staves";
    case 61:
        return "Sacred seals";
    case 65:
        return "Small shields";
    case 67:
        return "Medium shields";
    case 69:
        return "Greatshields";
    case 87:
        return "Torches";
    case 88:
        return "Hand-to-hand arts";
    case 89:
        return "Perfume bottles";
    case 90:
        return "Thrusting shields";
    case 91:
        return "Throwing blades";
    case 92:
        return "Backhand blades";
    case 93:
        return "Light greatswords";
    case 94:
        return "Great katanas";
    case 95:
        return "Beast claws";
    default:
        return "Other weapons";
    }
}

bool LoadWeaponCatalog(const std::wstring& dir, const std::string& language, WeaponCatalog& out, std::string& why) {
    const std::set<std::string> languages = {"engus", "frafr", "deude", "itait", "jpnjp", "korkr", "polpl",
                                             "porbr", "rusru", "spaar", "spaes", "thath", "zhocn", "zhotw"};
    if (!languages.count(language))
        return Fail(why, "unsupported catalog language");
    std::vector<uint8_t> regulation;
    std::vector<BinderEntry> entries;
    if (!ArchiveReadRegulation(regulation, why, dir) || !ReadWeaponBinder(regulation, entries, why))
        return false;
    std::vector<uint8_t> param;
    for (const auto& e : entries)
        if (e.name.find("EquipParamWeapon.param") != std::string::npos) {
            std::vector<uint8_t> raw(regulation.begin() + e.offset, regulation.begin() + e.offset + e.size);
            if (!ArchiveDecompress(raw, param, why, dir))
                return false;
            break;
        }
    if (param.empty())
        return Fail(why, "EquipParamWeapon is missing from regulation.bin");
    std::map<uint32_t, std::string> names;
    for (const char* binder : {"item.msgbnd.dcx", "item_dlc01.msgbnd.dcx", "item_dlc02.msgbnd.dcx"}) {
        const std::string path = "/msg/" + language + "/" + binder;
        std::vector<uint8_t> bytes;
        if (!ArchiveReadFile(path.c_str(), bytes, why, dir)) {
            if (names.empty())
                return false;
            continue;
        }
        if (!ReadWeaponBinder(bytes, entries, why))
            return false;
        for (const auto& e : entries) {
            // Internal binder filenames use Japanese names, independent of the selected language.
            if (e.name.find("\xE6\xAD\xA6\xE5\x99\xA8\xE5\x90\x8D") == std::string::npos &&
                e.name.find("WeaponName") == std::string::npos && e.id != 10)
                continue;
            std::vector<uint8_t> raw(bytes.begin() + e.offset, bytes.begin() + e.offset + e.size), fmg;
            if (!ArchiveDecompress(raw, fmg, why, dir) || !ReadWeaponNames(fmg, names, why))
                return false;
        }
    }
    if (!BuildWeaponCatalog(param, names, out, why))
        return false;
    out.language = language;
    why.clear();
    return true;
}
} // namespace ervr
