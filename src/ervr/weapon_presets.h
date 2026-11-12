#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ervr {
struct Config;
enum class WeaponField : uint8_t {
    Pitch,
    Yaw,
    Roll,
    GripX,
    GripY,
    GripZ,
    Reach,
    Balance,
    InertiaResponse,
    InertiaAcceleration,
    InertiaBrake,
    MaxLag,
    TwoHandTorque,
    ContactOn,
    ContactOff,
    ContactDwell,
    ContactMax,
    ContactCooldown,
    SwingOn,
    SwingOff,
    Heavy,
    MinHand,
    ArmWindow,
    SwingCooldown,
    GrabRadius,
    GrabMin,
    GrabMax,
    SkillHold,
    StanceSettle,
    StanceGap,
    AimClamp,
    ShieldHeight,
    ShieldForward,
    ShieldRelease,
    DeflectSpeed,
    HapticScale,
    Count
};
constexpr size_t kWeaponFieldCount = (size_t)WeaponField::Count;
struct WeaponFieldInfo {
    WeaponField field;
    const char* key;
    const char* label;
    const char* group;
    const char* unit;
    float min, max, step;
};
const std::array<WeaponFieldInfo, kWeaponFieldCount>& WeaponFields();
float WeaponFieldBaseline(WeaponField field, const Config& c, uint16_t type);
struct WeaponOverrides {
    uint64_t mask = 0;
    std::array<float, kWeaponFieldCount> values{};
    bool Has(WeaponField f) const { return (mask & (uint64_t{1} << (size_t)f)) != 0; }
    float Get(WeaponField f, float fallback) const { return Has(f) ? values[(size_t)f] : fallback; }
    void Set(WeaponField f, float value) {
        mask |= uint64_t{1} << (size_t)f;
        values[(size_t)f] = value;
    }
    void Reset(WeaponField f) {
        mask &= ~(uint64_t{1} << (size_t)f);
        values[(size_t)f] = 0;
    }
};
enum class WeaponEvent : uint8_t { Contact, Stance, Skill, Grip, Guard, Cast, Count };
enum class WeaponAction : uint8_t { Default, Suppress, Light, Heavy, GameSkill, EffectSkill, Count };
enum class WeaponCondition : uint8_t { Always, OneHand, TwoHand, Mounted, OnFoot, Count };
const char* WeaponEventName(WeaponEvent event);
const char* WeaponActionName(WeaponAction action);
const char* WeaponConditionName(WeaponCondition condition);
bool WeaponActionSupported(WeaponEvent event, WeaponAction action);
struct WeaponRule {
    std::string id, name;
    WeaponEvent event = WeaponEvent::Contact;
    WeaponAction action = WeaponAction::Default;
    WeaponCondition condition = WeaponCondition::Always;
    int priority = 50;
    int hand = -1; // -1 either hand; 0 left; 1 right
    bool enabled = true;
};
struct WeaponPreset {
    std::string id, name, author;
    uint32_t weapon = 0;
    uint16_t family = 0; // exact weapon wins; weapon 0 = family preset
    uint32_t revision = 1;
    WeaponOverrides tuning;
    std::vector<WeaponRule> rules;
};
struct WeaponAssignment {
    uint32_t weapon = 0;
    uint16_t family = 0;
    int hand = -1;
    std::string preset;
};
struct WeaponPresetStore {
    uint32_t revision = 0;
    std::vector<WeaponPreset> presets;
    std::vector<WeaponAssignment> assignments;
};
struct WeaponRuleContext {
    int hand = 1;
    bool twoHanded = false, mounted = false;
};
struct CompiledWeaponRule {
    WeaponEvent event{};
    WeaponAction action{};
    WeaponCondition condition{};
    int priority = 0, hand = -1;
    uint32_t order = 0;
};
struct ResolvedWeaponPreset {
    WeaponOverrides tuning;
    std::array<CompiledWeaponRule, 32> rules{};
    uint32_t ruleCount = 0, revision = 0;
    uint64_t fingerprint = 0;
};
WeaponAction DecideWeaponAction(const ResolvedWeaponPreset& preset, WeaponEvent event, const WeaponRuleContext& context,
                                int* matched = nullptr);
const WeaponPreset* AssignedWeaponPreset(const WeaponPresetStore& store, uint32_t weapon, uint16_t family, int hand);
ResolvedWeaponPreset CompileWeaponPreset(const WeaponPreset& preset);
ResolvedWeaponPreset ResolveWeaponPreset(const WeaponPresetStore& store, uint32_t weapon, uint16_t family, int hand);
bool ValidateWeaponPreset(const WeaponPreset& preset, std::string& why);
bool ValidateWeaponStore(const WeaponPresetStore& store, std::string& why);
std::string NewWeaponPresetId();
std::string WeaponPresetJson(const WeaponPreset& preset);
bool ParseWeaponPreset(std::string_view text, WeaponPreset& preset, std::string& why);
std::string WeaponShareCode(const WeaponPreset& preset);
bool ParseWeaponShare(std::string_view text, WeaponPreset& preset, std::string& why);
bool LoadWeaponPresetStore(const std::filesystem::path& file, WeaponPresetStore& store, std::string& why);
// Revision checked under a file lock; temp + atomic replace. A conflicting writer is never overwritten.
bool SaveWeaponPresetStore(const std::filesystem::path& file, WeaponPresetStore& store, uint32_t expectedRevision,
                           std::string& why);
} // namespace ervr
