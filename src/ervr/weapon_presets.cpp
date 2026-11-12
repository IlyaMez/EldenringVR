#include "weapon_presets.h"
#include "config.h"
#include <json/json.h>
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>

namespace ervr {
const std::array<WeaponFieldInfo, kWeaponFieldCount>& WeaponFields() {
    static const std::array<WeaponFieldInfo, kWeaponFieldCount> fields = {
        {{WeaponField::Pitch, "pitch", "Pitch correction", "Fit", "deg", -90, 90, 1},
         {WeaponField::Yaw, "yaw", "Yaw correction", "Fit", "deg", -90, 90, 1},
         {WeaponField::Roll, "roll", "Roll correction", "Fit", "deg", -180, 180, 1},
         {WeaponField::GripX, "gripX", "Grip sideways", "Fit", "m", -.3f, .3f, .005f},
         {WeaponField::GripY, "gripY", "Grip height", "Fit", "m", -.3f, .3f, .005f},
         {WeaponField::GripZ, "gripZ", "Grip forward", "Fit", "m", -.3f, .3f, .005f},
         {WeaponField::Reach, "reach", "Grip to tip estimate", "Fit", "m", .05f, 3, .01f},
         {WeaponField::Balance, "balance", "Balance point fraction", "Fit", "", .01f, 1, .01f},
         {WeaponField::InertiaResponse, "inertiaResponse", "Angular response", "Weight", "Hz", .5f, 30, .5f},
         {WeaponField::InertiaAcceleration, "inertiaAcceleration", "Angular acceleration", "Weight", "rad/s2", 10, 1000,
          10},
         {WeaponField::InertiaBrake, "inertiaBrake", "Braking assistance", "Weight", "x", 1, 10, .1f},
         {WeaponField::MaxLag, "maxLag", "Maximum angular lag", "Weight", "deg", 1, 45, 1},
         {WeaponField::TwoHandTorque, "twoHandTorque", "Two-hand assistance", "Weight", "x", .1f, 8, .1f},
         {WeaponField::ContactOn, "contactOn", "Contact begins", "Contact", "m/s", .1f, 8, .1f},
         {WeaponField::ContactOff, "contactOff", "Contact ends below", "Contact", "m/s", .01f, 4, .05f},
         {WeaponField::ContactDwell, "contactDwell", "Slowdown dwell", "Contact", "s", .02f, .5f, .01f},
         {WeaponField::ContactMax, "contactMax", "Maximum stroke duration", "Contact", "s", .1f, 2, .05f},
         {WeaponField::ContactCooldown, "contactCooldown", "Recovery after hit", "Contact", "s", .05f, 1, .01f},
         {WeaponField::SwingOn, "swingOn", "Stance / gated swing begins", "Gesture", "m/s", .5f, 12, .1f},
         {WeaponField::SwingOff, "swingOff", "Swing rest threshold", "Gesture", "m/s", .05f, 6, .05f},
         {WeaponField::Heavy, "heavy", "Heavy follow-up threshold", "Gesture", "m/s", .5f, 16, .1f},
         {WeaponField::MinHand, "minHand", "Minimum hand speed", "Gesture", "m/s", .1f, 4, .1f},
         {WeaponField::ArmWindow, "armWindow", "Classification window", "Gesture", "s", .02f, .3f, .01f},
         {WeaponField::SwingCooldown, "swingCooldown", "Gated swing recovery", "Gesture", "s", .05f, 1, .01f},
         {WeaponField::GrabRadius, "grabRadius", "Support grip radius", "Grip", "m", .04f, .35f, .01f},
         {WeaponField::GrabMin, "grabMin", "Hilt segment start", "Grip", "m", -.4f, .5f, .01f},
         {WeaponField::GrabMax, "grabMax", "Hilt segment end", "Grip", "m", -.3f, 1, .01f},
         {WeaponField::SkillHold, "skillHold", "Hold before game skill", "Skill", "s", .05f, 1, .01f},
         {WeaponField::StanceSettle, "stanceSettle", "Stance settling time", "Skill", "s", .05f, .8f, .01f},
         {WeaponField::StanceGap, "stanceGap", "Stance follow-up gap", "Skill", "s", .1f, 1.5f, .01f},
         {WeaponField::AimClamp, "aimClamp", "Swing aim limit", "Skill", "deg", 5, 90, 1},
         {WeaponField::ShieldHeight, "shieldHeight", "Guard height below eyes", "Guard", "m", -.8f, 0, .01f},
         {WeaponField::ShieldForward, "shieldForward", "Guard forward distance", "Guard", "m", 0, .6f, .01f},
         {WeaponField::ShieldRelease, "shieldRelease", "Guard release delay", "Guard", "s", .05f, .7f, .01f},
         {WeaponField::DeflectSpeed, "deflectSpeed", "Deflect speed", "Guard", "m/s", .2f, 4, .1f},
         {WeaponField::HapticScale, "hapticScale", "Controller feedback", "Feedback", "x", 0, 1, .05f}}};
    return fields;
}
float WeaponFieldBaseline(WeaponField f, const Config& c, uint16_t type) {
    if (f == WeaponField::Reach || f == WeaponField::Balance) {
        size_t at = 0;
        while (at < c.weaponGeometry.size()) {
            int category = 0;
            float reach = 0, balance = 0;
            if (sscanf_s(c.weaponGeometry.c_str() + at, "%d:%f:%f", &category, &reach, &balance) == 3 &&
                category == type && reach > .05f)
                return f == WeaponField::Reach ? reach : balance;
            const auto end = c.weaponGeometry.find(',', at);
            if (end == std::string::npos)
                break;
            at = end + 1;
        }
    }
    switch (f) {
    case WeaponField::Pitch:
    case WeaponField::Yaw:
    case WeaponField::Roll:
    case WeaponField::GripX:
    case WeaponField::GripY:
    case WeaponField::GripZ:
        return 0;
    case WeaponField::Reach:
        switch (type) {
        case 1:
            return .35f;
        case 3:
            return .95f;
        case 5:
            return 1.25f;
        case 7:
            return 1.6f;
        case 9:
            return .85f;
        case 11:
            return 1.2f;
        case 14:
            return .7f;
        case 16:
            return 1.2f;
        case 17:
        case 21:
            return .7f;
        case 19:
        case 23:
            return 1.3f;
        case 24:
            return .8f;
        case 25:
            return 1.6f;
        case 28:
            return 1.9f;
        case 29:
            return 1.8f;
        case 31:
            return 1.7f;
        case 35:
        case 88:
            return .15f;
        case 37:
        case 65:
        case 89:
            return .3f;
        case 39:
            return 1.5f;
        case 41:
            return 1.6f;
        case 57:
            return 1.2f;
        case 61:
            return .2f;
        case 67:
            return .45f;
        case 69:
            return .8f;
        case 87:
            return .6f;
        case 90:
        case 92:
            return .7f;
        case 91:
        case 95:
            return .4f;
        case 93:
            return 1.15f;
        case 94:
            return 1.4f;
        default:
            return 1;
        }
    case WeaponField::Balance:
        switch (type) {
        case 3:
        case 13:
        case 69:
        case 93:
            return .35f;
        case 7:
        case 11:
        case 28:
            return .45f;
        case 14:
            return .1f;
        case 15:
        case 35:
        case 37:
        case 39:
        case 50:
        case 51:
        case 53:
        case 55:
        case 56:
        case 61:
        case 88:
            return .3f;
        case 17:
        case 19:
        case 41:
            return .75f;
        case 21:
            return .8f;
        case 23:
        case 24:
            return .85f;
        case 29:
        case 87:
            return .6f;
        case 31:
            return .65f;
        case 57:
        case 89:
        case 91:
            return .5f;
        case 65:
            return .15f;
        case 67:
            return .2f;
        default:
            return .4f;
        }
    case WeaponField::InertiaResponse:
        return c.inertiaSpringHz;
    case WeaponField::InertiaAcceleration:
        return c.inertiaAngAccel;
    case WeaponField::InertiaBrake:
        return c.inertiaBrake;
    case WeaponField::MaxLag:
        return c.inertiaMaxLagAngle * 57.2957795f;
    case WeaponField::TwoHandTorque:
        return c.inertiaTwoHandTorque;
    case WeaponField::ContactOn:
        return c.contactOnSpeed;
    case WeaponField::ContactOff:
        return c.contactOffSpeed;
    case WeaponField::ContactDwell:
        return c.contactDwellSeconds;
    case WeaponField::ContactMax:
        return c.contactMaxSeconds;
    case WeaponField::ContactCooldown:
        return c.contactCooldownSeconds;
    case WeaponField::SwingOn:
        return c.swingOnSpeed;
    case WeaponField::SwingOff:
        return c.swingOffSpeed;
    case WeaponField::Heavy:
        return c.heavySwingSpeed;
    case WeaponField::MinHand:
        return c.minHandSpeed;
    case WeaponField::ArmWindow:
        return c.swingArmWindow;
    case WeaponField::SwingCooldown:
        return c.swingCooldown;
    case WeaponField::GrabRadius:
        return c.twoHandGrabRadius;
    case WeaponField::GrabMin:
        return c.twoHandGrabMin;
    case WeaponField::GrabMax:
        return c.twoHandGrabMax;
    case WeaponField::SkillHold:
        return c.skillHoldSeconds;
    case WeaponField::StanceSettle:
        return .15f;
    case WeaponField::StanceGap:
        return .5f;
    case WeaponField::AimClamp:
        return c.swingAimMaxDeg;
    case WeaponField::ShieldHeight:
        return c.shieldUpHeight;
    case WeaponField::ShieldForward:
        return c.shieldUpForward;
    case WeaponField::ShieldRelease:
        return c.blockReleaseSeconds;
    case WeaponField::DeflectSpeed:
        return c.deflectSpeed;
    case WeaponField::HapticScale:
        return 1;
    default:
        return 0;
    }
}
const char* WeaponEventName(WeaponEvent e) {
    static const char* names[] = {"Blade contact", "Stance swing",  "Skill control",
                                  "Support grip",  "Shield raised", "Cast / shoot"};
    return (size_t)e < 6 ? names[(size_t)e] : "Unknown";
}
const char* WeaponActionName(WeaponAction a) {
    static const char* names[] = {"Use game / ERVR behavior", "Suppress action", "Light attack / follow-up",
                                  "Heavy attack / follow-up", "Use game skill",  "Use effect skill"};
    return (size_t)a < 6 ? names[(size_t)a] : "Unknown";
}
const char* WeaponConditionName(WeaponCondition c) {
    static const char* names[] = {"Always", "One-handed", "Two-handed", "Mounted", "On foot"};
    return (size_t)c < 5 ? names[(size_t)c] : "Unknown";
}
bool WeaponActionSupported(WeaponEvent e, WeaponAction a) {
    return a == WeaponAction::Default || a == WeaponAction::Suppress ||
           ((e == WeaponEvent::Contact || e == WeaponEvent::Stance) &&
            (a == WeaponAction::Light || a == WeaponAction::Heavy)) ||
           (e == WeaponEvent::Skill && (a == WeaponAction::GameSkill || a == WeaponAction::EffectSkill));
}
namespace {
bool Matches(WeaponCondition c, const WeaponRuleContext& x) {
    switch (c) {
    case WeaponCondition::Always:
        return true;
    case WeaponCondition::OneHand:
        return !x.twoHanded;
    case WeaponCondition::TwoHand:
        return x.twoHanded;
    case WeaponCondition::Mounted:
        return x.mounted;
    case WeaponCondition::OnFoot:
        return !x.mounted;
    default:
        return false;
    }
}
bool ValidText(const std::string& s, size_t max, bool empty = false) {
    return s.size() <= max && (empty || !s.empty()) && s.find('\0') == std::string::npos &&
           (s.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0) > 0);
}
bool Id(const std::string& s) {
    return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}
bool Fail(std::string& w, const std::string& s) {
    w = s;
    return false;
}
bool Fields(const Json::Value& v, std::initializer_list<const char*> keys) {
    if (!v.isObject())
        return false;
    for (const auto& k : v.getMemberNames()) {
        bool found = false;
        for (const auto* allowed : keys)
            if (k == allowed)
                found = true;
        if (!found)
            return false;
    }
    return true;
}
bool Parse(std::string_view text, Json::Value& out, std::string& why, size_t limit) {
    if (text.empty() || text.size() > limit ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0) <= 0)
        return Fail(why, "Invalid UTF-8 or input exceeds the size limit.");
    Json::CharReaderBuilder b;
    Json::CharReaderBuilder::strictMode(&b.settings_);
    b["stackLimit"] = 16;
    b["allowComments"] = false;
    b["allowTrailingCommas"] = false;
    try {
        std::unique_ptr<Json::CharReader> reader(b.newCharReader());
        return reader->parse(text.data(), text.data() + text.size(), &out, &why);
    } catch (const std::exception&) {
        return Fail(why, "JSON nesting exceeds the limit.");
    }
}
std::string Write(const Json::Value& v) {
    Json::StreamWriterBuilder b;
    b["indentation"] = "  ";
    return Json::writeString(b, v);
}
Json::Value ToJson(const WeaponPreset& p) {
    Json::Value j(Json::objectValue);
    j["format"] = "ervr.weapon-preset";
    j["schemaVersion"] = 1;
    j["id"] = p.id;
    j["name"] = p.name;
    j["author"] = p.author;
    j["revision"] = p.revision;
    j["weapon"] = p.weapon;
    j["family"] = p.family;
    j["tuning"] = Json::Value(Json::objectValue);
    for (const auto& f : WeaponFields())
        if (p.tuning.Has(f.field))
            j["tuning"][f.key] = p.tuning.values[(size_t)f.field];
    j["rules"] = Json::Value(Json::arrayValue);
    for (const auto& r : p.rules) {
        Json::Value v;
        v["id"] = r.id;
        v["name"] = r.name;
        v["event"] = (unsigned)r.event;
        v["action"] = (unsigned)r.action;
        v["condition"] = (unsigned)r.condition;
        v["priority"] = r.priority;
        v["hand"] = r.hand;
        v["enabled"] = r.enabled;
        j["rules"].append(v);
    }
    return j;
}
bool FromJson(const Json::Value& j, WeaponPreset& p, std::string& why) {
    if (!Fields(j, {"format", "schemaVersion", "id", "name", "author", "revision", "weapon", "family", "tuning",
                    "rules"}) ||
        j["format"] != "ervr.weapon-preset" || j["schemaVersion"] != 1 || !j["id"].isString() ||
        !j["name"].isString() || !j["author"].isString() || !j["revision"].isUInt() || !j["weapon"].isUInt() ||
        !j["family"].isUInt() || j["family"].asUInt() > 65535 || !j["tuning"].isObject() || !j["rules"].isArray() ||
        j["rules"].size() > 32)
        return Fail(why, "Unsupported or invalid preset schema.");
    WeaponPreset v;
    v.id = j["id"].asString();
    v.name = j["name"].asString();
    v.author = j["author"].asString();
    v.revision = j["revision"].asUInt();
    v.weapon = j["weapon"].asUInt();
    v.family = (uint16_t)j["family"].asUInt();
    for (const auto& key : j["tuning"].getMemberNames()) {
        const WeaponFieldInfo* field = nullptr;
        for (const auto& f : WeaponFields())
            if (key == f.key)
                field = &f;
        if (!field || !j["tuning"][key].isNumeric())
            return Fail(why, "Unknown tuning field: " + key);
        v.tuning.Set(field->field, j["tuning"][key].asFloat());
    }
    for (const auto& r : j["rules"]) {
        if (!Fields(r, {"id", "name", "event", "action", "condition", "priority", "hand", "enabled"}) ||
            !r["id"].isString() || !r["name"].isString() || !r["event"].isUInt() ||
            r["event"].asUInt() >= (unsigned)WeaponEvent::Count || !r["action"].isUInt() ||
            r["action"].asUInt() >= (unsigned)WeaponAction::Count || !r["condition"].isUInt() ||
            r["condition"].asUInt() >= (unsigned)WeaponCondition::Count || !r["priority"].isInt() ||
            !r["hand"].isInt() || !r["enabled"].isBool())
            return Fail(why, "Invalid action rule.");
        v.rules.push_back({r["id"].asString(), r["name"].asString(), (WeaponEvent)r["event"].asUInt(),
                           (WeaponAction)r["action"].asUInt(), (WeaponCondition)r["condition"].asUInt(),
                           r["priority"].asInt(), r["hand"].asInt(), r["enabled"].asBool()});
    }
    if (!ValidateWeaponPreset(v, why))
        return false;
    p = std::move(v);
    return true;
}
std::string Hash(std::string_view text) {
    uint8_t hash[32]{};
    BCRYPT_ALG_HANDLE a = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(a, &h, nullptr, 0, nullptr, 0, 0));
    ok = ok && BCRYPT_SUCCESS(BCryptHashData(h, (PUCHAR)text.data(), (ULONG)text.size(), 0)) &&
         BCRYPT_SUCCESS(BCryptFinishHash(h, hash, 32, 0));
    if (h)
        BCryptDestroyHash(h);
    if (a)
        BCryptCloseAlgorithmProvider(a, 0);
    if (!ok)
        return {};
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (uint8_t b : hash) {
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    return out;
}
} // namespace
bool ValidateWeaponPreset(const WeaponPreset& p, std::string& why) {
    if (!Id(p.id) || !ValidText(p.name, 96) || !ValidText(p.author, 96, true) || !p.revision || p.weapon >= 100000000 ||
        (!p.weapon && !p.family) || p.weapon % 100 || p.rules.size() > 32 || (p.tuning.mask >> kWeaponFieldCount))
        return Fail(why, "Invalid preset identity, target or rule count.");
    for (const auto& f : WeaponFields())
        if (p.tuning.Has(f.field)) {
            const float v = p.tuning.values[(size_t)f.field];
            if (!std::isfinite(v) || v < f.min || v > f.max)
                return Fail(why, std::string(f.label) + " is outside its supported range.");
        }
    auto ordered = [&](WeaponField a, WeaponField b, bool equal) {
        return !p.tuning.Has(a) || !p.tuning.Has(b) ||
               (equal ? p.tuning.Get(a, 0) <= p.tuning.Get(b, 0) : p.tuning.Get(a, 0) < p.tuning.Get(b, 0));
    };
    if (!ordered(WeaponField::ContactOff, WeaponField::ContactOn, false) ||
        !ordered(WeaponField::SwingOff, WeaponField::SwingOn, false) ||
        !ordered(WeaponField::SwingOn, WeaponField::Heavy, true) ||
        !ordered(WeaponField::GrabMin, WeaponField::GrabMax, false))
        return Fail(why, "Thresholds or grip bounds are reversed.");
    std::set<std::string> ids;
    for (const auto& r : p.rules) {
        if (!Id(r.id) || !ids.insert(r.id).second || !ValidText(r.name, 96) || r.hand < -1 || r.hand > 1 ||
            r.priority < 0 || r.priority > 100 || (size_t)r.event >= (size_t)WeaponEvent::Count ||
            (size_t)r.condition >= (size_t)WeaponCondition::Count || !WeaponActionSupported(r.event, r.action))
            return Fail(why, "Invalid or unsupported action rule.");
    }
    for (size_t i = 0; i < p.rules.size(); ++i)
        for (size_t j = i + 1; j < p.rules.size(); ++j) {
            const auto& a = p.rules[i];
            const auto& b = p.rules[j];
            if (!a.enabled || !b.enabled || a.event != b.event || a.priority != b.priority ||
                (a.hand >= 0 && b.hand >= 0 && a.hand != b.hand))
                continue;
            for (int two = 0; two < 2; ++two)
                for (int mounted = 0; mounted < 2; ++mounted)
                    if (Matches(a.condition, {1, two != 0, mounted != 0}) &&
                        Matches(b.condition, {1, two != 0, mounted != 0}))
                        return Fail(why, "Overlapping rules have the same priority.");
        }
    why.clear();
    return true;
}
bool ValidateWeaponStore(const WeaponPresetStore& s, std::string& why) {
    if (s.presets.size() > 1024 || s.assignments.size() > 2048)
        return Fail(why, "Preset library is full.");
    std::set<std::string> ids, targets;
    for (const auto& p : s.presets)
        if (!ValidateWeaponPreset(p, why) || !ids.insert(p.id).second)
            return Fail(why, why.empty() ? "Duplicate preset ID." : why);
    for (const auto& a : s.assignments) {
        const auto it =
            std::find_if(s.presets.begin(), s.presets.end(), [&](const auto& p) { return p.id == a.preset; });
        if (it == s.presets.end() || a.hand < -1 || a.hand > 1 || it->weapon != a.weapon || it->family != a.family ||
            !targets
                 .insert(std::to_string(a.weapon) + ":" + std::to_string(a.weapon ? 0 : a.family) + ":" +
                         std::to_string(a.hand))
                 .second)
            return Fail(why, "Invalid or duplicate active assignment.");
    }
    return true;
}
const WeaponPreset* AssignedWeaponPreset(const WeaponPresetStore& s, uint32_t weapon, uint16_t family, int hand) {
    const WeaponPreset* selected = nullptr;
    int best = -1;
    for (const auto& a : s.assignments) {
        if (a.hand != -1 && a.hand != hand)
            continue;
        if (a.weapon ? a.weapon != weapon : a.family != family)
            continue;
        int rank = (a.weapon ? 2 : 0) + (a.hand != -1 ? 1 : 0);
        if (rank <= best)
            continue;
        for (const auto& p : s.presets)
            if (p.id == a.preset) {
                selected = &p;
                best = rank;
                break;
            }
    }
    return selected;
}
ResolvedWeaponPreset ResolveWeaponPreset(const WeaponPresetStore& s, uint32_t weapon, uint16_t family, int hand) {
    const auto* selected = AssignedWeaponPreset(s,weapon,family,hand);
    return selected ? CompileWeaponPreset(*selected) : ResolvedWeaponPreset{};
}
ResolvedWeaponPreset CompileWeaponPreset(const WeaponPreset& preset) {
    const auto* selected = &preset;
    ResolvedWeaponPreset out;
    out.tuning = selected->tuning;
    out.revision = selected->revision;
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char ch : selected->id) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    hash ^= selected->revision;
    out.fingerprint = hash;
    auto rules = selected->rules;
    std::sort(rules.begin(), rules.end(), [](const auto& a, const auto& b) {
        return a.priority != b.priority ? a.priority > b.priority : a.id < b.id;
    });
    for (const auto& r : rules)
        if (r.enabled && out.ruleCount < out.rules.size()) {
            out.rules[out.ruleCount] = {r.event, r.action, r.condition, r.priority, r.hand, out.ruleCount};
            ++out.ruleCount;
        }
    return out;
}
WeaponAction DecideWeaponAction(const ResolvedWeaponPreset& p, WeaponEvent event, const WeaponRuleContext& ctx,
                                int* matched) {
    if (matched)
        *matched = -1;
    for (uint32_t i = 0; i < p.ruleCount; ++i) {
        const auto& r = p.rules[i];
        if (r.event == event && (r.hand < 0 || r.hand == ctx.hand) && Matches(r.condition, ctx)) {
            if (matched)
                *matched = (int)i;
            return r.action;
        }
    }
    return WeaponAction::Default;
}
std::string NewWeaponPresetId() {
    uint8_t bytes[16];
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return {};
    std::string id;
    static const char* hex = "0123456789abcdef";
    for (uint8_t b : bytes) {
        id += hex[b >> 4];
        id += hex[b & 15];
    }
    return id;
}
std::string WeaponPresetJson(const WeaponPreset& p) {
    return Write(ToJson(p));
}
bool ParseWeaponPreset(std::string_view text, WeaponPreset& p, std::string& why) {
    Json::Value j;
    return Parse(text, j, why, 65536) && FromJson(j, p, why);
}
std::string WeaponShareCode(const WeaponPreset& p) {
    const std::string text = WeaponPresetJson(p), hash = Hash(text);
    if (hash.empty())
        return {};
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string code = "ERVR1:";
    uint32_t acc = 0;
    int bits = 0;
    for (uint8_t c : text) {
        acc = (acc << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            code += alphabet[(acc >> bits) & 63];
        }
    }
    if (bits)
        code += alphabet[(acc << (6 - bits)) & 63];
    return code + ":" + hash;
}
bool ParseWeaponShare(std::string_view text, WeaponPreset& p, std::string& why) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\n' || text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r'))
        text.remove_suffix(1);
    if (!text.starts_with("ERVR1:"))
        return ParseWeaponPreset(text, p, why);
    if (text.size() > 90000)
        return Fail(why, "Share code exceeds the size limit.");
    const size_t end = text.find(':', 6);
    if (end == std::string_view::npos || text.size() - end != 65)
        return Fail(why, "Invalid share code envelope.");
    std::string raw;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text.substr(6, end - 6)) {
        int n = c >= 'A' && c <= 'Z'   ? c - 'A'
                : c >= 'a' && c <= 'z' ? c - 'a' + 26
                : c >= '0' && c <= '9' ? c - '0' + 52
                : c == '-'             ? 62
                : c == '_'             ? 63
                                       : -1;
        if (n < 0)
            return Fail(why, "Invalid share code characters.");
        acc = (acc << 6) | (uint32_t)n;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            raw += (char)(acc >> bits);
        }
    }
    if (bits >= 6 || (bits && (acc & ((1u << bits) - 1))))
        return Fail(why, "Truncated share code.");
    if (Hash(raw) != text.substr(end + 1))
        return Fail(why, "Share code checksum failed.");
    return ParseWeaponPreset(raw, p, why);
}
bool LoadWeaponPresetStore(const std::filesystem::path& file, WeaponPresetStore& store, std::string& why) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) && !ec) {
        store = {};
        why.clear();
        return true;
    }
    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size > (4u << 20))
        return Fail(why, "Could not read the preset library (or it exceeds 4 MiB).");
    std::ifstream f(file, std::ios::binary);
    std::string text((size_t)size, '\0');
    if (!f.read(text.data(), (std::streamsize)text.size()) || f.peek() != std::char_traits<char>::eof())
        return Fail(why, "The preset library changed while reading it. Retry.");
    Json::Value j;
    if (!Parse(text, j, why, 4u << 20))
        return false;
    if (!Fields(j, {"format", "schemaVersion", "revision", "presets", "assignments"}) ||
        j["format"] != "ervr.weapon-library" || j["schemaVersion"] != 1 || !j["revision"].isUInt() ||
        !j["presets"].isArray() || j["presets"].size() > 1024 || !j["assignments"].isArray() ||
        j["assignments"].size() > 2048)
        return Fail(why, "Unsupported preset library schema.");
    WeaponPresetStore s;
    s.revision = j["revision"].asUInt();
    for (const auto& p : j["presets"]) {
        WeaponPreset v;
        if (!FromJson(p, v, why))
            return false;
        s.presets.push_back(std::move(v));
    }
    for (const auto& a : j["assignments"]) {
        if (!Fields(a, {"weapon", "family", "hand", "preset"}) || !a["weapon"].isUInt() || !a["family"].isUInt() ||
            a["family"].asUInt() > 65535 || !a["hand"].isInt() || !a["preset"].isString())
            return Fail(why, "Invalid preset assignment.");
        s.assignments.push_back(
            {a["weapon"].asUInt(), (uint16_t)a["family"].asUInt(), a["hand"].asInt(), a["preset"].asString()});
    }
    if (!ValidateWeaponStore(s, why))
        return false;
    store = std::move(s);
    why.clear();
    return true;
}
bool SaveWeaponPresetStore(const std::filesystem::path& file, WeaponPresetStore& store, uint32_t expected,
                           std::string& why) {
    if (!ValidateWeaponStore(store, why))
        return false;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec)
        return Fail(why, "The preset folder is not writable.");
    const auto lockPath = file.wstring() + L".lock";
    HANDLE lock = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock == INVALID_HANDLE_VALUE)
        return Fail(why, "Another editor is saving presets. Retry after it finishes.");
    struct Close {
        HANDLE h;
        ~Close() { CloseHandle(h); }
    } close{lock};
    WeaponPresetStore disk;
    if (!LoadWeaponPresetStore(file, disk, why))
        return false;
    if (disk.revision != expected)
        return Fail(why, "The library changed in another editor. Reload before saving; your draft is preserved.");
    if (expected == UINT32_MAX)
        return Fail(why, "Library revision exhausted.");
    Json::Value j;
    j["format"] = "ervr.weapon-library";
    j["schemaVersion"] = 1;
    j["revision"] = expected + 1;
    j["presets"] = Json::Value(Json::arrayValue);
    j["assignments"] = Json::Value(Json::arrayValue);
    for (const auto& p : store.presets)
        j["presets"].append(ToJson(p));
    for (const auto& a : store.assignments) {
        Json::Value v;
        v["weapon"] = a.weapon;
        v["family"] = a.family;
        v["hand"] = a.hand;
        v["preset"] = a.preset;
        j["assignments"].append(v);
    }
    const auto data = Write(j);
    if (data.size() > (4u << 20))
        return Fail(why, "Preset library exceeds 4 MiB.");
    const auto temp = file.wstring() + L".tmp";
    HANDLE out = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE)
        return Fail(why, "Could not write the preset temporary file.");
    DWORD written = 0;
    const bool ok = WriteFile(out, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size() &&
                    FlushFileBuffers(out);
    CloseHandle(out);
    if (!ok || !MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return Fail(why, "Could not replace the preset library; previous settings are intact.");
    }
    store.revision = expected + 1;
    why.clear();
    return true;
}
} // namespace ervr
