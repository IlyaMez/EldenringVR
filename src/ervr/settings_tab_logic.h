#pragma once
// The ERVR tab's rows: which ERVR.ini key each shows, the choices it offers and what choosing one writes,
// kept free of game state so tools/test_settings_tab.cpp can drive them. settings_tab.cpp builds the tab.
//
// The page draws in Sound's window, which has 12 rows (a list longer than its window scrolls the cursor but
// not the rows), so the tab has the 12 settings players change most while playing; the launcher's Options and
// ERVR.ini have the rest (Resolution among them: it needs a restart, so its row went to the body calibration).
// Every row here is read live by ERVR, so a change applies within a second (the ini hot-reload).
#include "body_calibration_logic.h"
#include "launcher_text.hpp"
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

namespace ervr {

constexpr int kTabMaxRows = 12;      // AudioSetting's rows (the page itself would take 16)
constexpr int kTabMaxChoices = 32;   // a list row: ComboItemList<unsigned char, 32>

// calibrate: the body calibration ([Body], body_calibration.h), a list of two built from the ini - what it holds,
// then the T-pose again. Choosing the second asks for the panel at the next moment of play (the menu holds it
// until it closes); choosing the first again before that takes the ask back. It writes nothing itself: the
// calibration does, when it is done.
enum class TabRowKind : uint8_t { onOff, list, calibrate };

struct TabChoice {
    const wchar_t* label;
    const char* value;
    const char* value2 = nullptr;   // the row's second key, for choices that set two (Turning)
};

struct TabRow {
    TabRowKind kind;
    const char* section;
    const char* key;
    const wchar_t* title;
    const wchar_t* help;
    const char* fallback;           // what ERVR uses with the key absent (config.h's default)
    std::vector<TabChoice> choices; // list rows
    const char* key2 = nullptr;
    const char* fallback2 = nullptr;
};

// ERVR's reading of a switch (config.cpp GetB): 1, true, yes or on, in any case; anything else is off.
inline bool TabTruthy(std::string_view v) {
    v = launcher::text::trim(v);
    return v == "1" || launcher::text::sameText(v, "true") || launcher::text::sameText(v, "yes") ||
           launcher::text::sameText(v, "on");
}
inline bool TabNumber(std::string_view v, double& out) {
    const std::string s(launcher::text::trim(v));
    if (s.empty())
        return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == '\0' && std::isfinite(out);
}
// Whether the ini's text is a choice's value: a switch by its meaning, a number by its size ("1" is "1.0"),
// a word without regard to case.
inline bool TabSame(std::string_view choice, std::string_view ini) {
    if (choice == "true" || choice == "false")
        return TabTruthy(ini) == (choice == "true");
    double a = 0, b = 0;
    if (TabNumber(choice, a) && TabNumber(ini, b))
        return std::fabs(a - b) <= 1e-4 * std::fmax(1.0, std::fabs(a));
    return launcher::text::sameText(launcher::text::trim(choice), launcher::text::trim(ini));
}

// One row as the tab shows it. On/off rows show the game's own OFF and ON: index 1 is on.
struct TabShown {
    std::vector<std::wstring> labels;   // list rows: the choices, then the ini's own value when it is none of them
    std::vector<std::string> values, values2;
    int current = 0;                    // what ERVR.ini says now
    int fallback = 0;                   // ERVR's default (the game's RESET)
};

struct TabWrite {
    std::string section, key, value;
};

namespace detail {
inline std::string TabIniValue(std::string_view ini, const char* section, const char* key, const char* fallback) {
    const auto v = launcher::text::iniGet(ini, section, key);
    return v && !launcher::text::trim(*v).empty() ? std::string(launcher::text::trim(*v)) : std::string(fallback ? fallback : "");
}
inline int TabFind(const TabShown& s, const TabRow& row, const std::string& v, const std::string& v2) {
    for (size_t i = 0; i < s.values.size(); ++i)
        if (TabSame(s.values[i], v) && (s.values2[i].empty() || !row.key2 || TabSame(s.values2[i], v2)))
            return (int)i;
    return -1;
}
} // namespace detail

// The calibrate row: what [Body] holds, as the launcher says it (height = eyes / 0.936), then the T-pose again.
inline TabShown TabShowCalibration(std::string_view ini) {
    TabShown s;
    const int eye = std::atoi(detail::TabIniValue(ini, "Body", "EyeHeightMm", "0").c_str());
    const int arm = std::atoi(detail::TabIniValue(ini, "Body", "ArmLengthMm", "0").c_str());
    const bool calibrated = CalibValid(eye, arm);
    if (calibrated) {
        wchar_t text[64];
        std::swprintf(text, 64, L"%.2f m tall, arms %.2f m", CalibHeight(eye / 1000.f), arm / 1000.0);
        s.labels.emplace_back(text);
    } else {
        s.labels.emplace_back(TabTruthy(detail::TabIniValue(ini, "Body", "CalibrationPrompt", "true")) ? L"Not yet"
                                                                                                       : L"Skipped");
    }
    s.labels.emplace_back(calibrated ? L"Redo when the menu closes" : L"Calibrate when the menu closes");
    s.values = {"", "ask"};
    s.values2 = {"", ""};
    return s;   // current and fallback 0: the row always opens on what the ini holds
}

inline TabShown TabShow(const TabRow& row, std::string_view ini) {
    if (row.kind == TabRowKind::calibrate)
        return TabShowCalibration(ini);
    TabShown s;
    const std::string v = detail::TabIniValue(ini, row.section, row.key, row.fallback);
    if (row.kind == TabRowKind::onOff) {
        s.current = TabTruthy(v) ? 1 : 0;
        s.fallback = TabTruthy(row.fallback) ? 1 : 0;
        return s;
    }
    for (const TabChoice& c : row.choices) {
        s.labels.emplace_back(c.label);
        s.values.emplace_back(c.value);
        s.values2.emplace_back(c.value2 ? c.value2 : "");
    }
    const std::string v2 = row.key2 ? detail::TabIniValue(ini, row.section, row.key2, row.fallback2) : std::string();
    s.fallback = (std::max)(0, detail::TabFind(s, row, row.fallback, row.fallback2 ? row.fallback2 : ""));
    s.current = detail::TabFind(s, row, v, v2);
    if (s.current < 0) {
        // A value the list does not offer shows as itself, last, and choosing it again writes it back.
        std::string shown = row.key2 && TabTruthy(v) ? v2 : v;
        s.labels.emplace_back(std::wstring(shown.begin(), shown.end()) + L" (ERVR.ini)");
        s.values.push_back(v);
        s.values2.push_back(row.key2 ? v2 : std::string());
        s.current = (int)s.values.size() - 1;
    }
    return s;
}

// The ini edits that choosing `index` on the row makes (none on the calibrate row: see TabAsksCalibration).
inline std::vector<TabWrite> TabWrites(const TabRow& row, const TabShown& s, int index) {
    std::vector<TabWrite> out;
    if (row.kind == TabRowKind::calibrate)
        return out;
    if (row.kind == TabRowKind::onOff) {
        out.push_back({row.section, row.key, index ? "true" : "false"});
        return out;
    }
    if (index < 0 || index >= (int)s.values.size())
        return out;
    out.push_back({row.section, row.key, s.values[(size_t)index]});
    if (row.key2 && !s.values2[(size_t)index].empty())
        out.push_back({row.section, row.key2, s.values2[(size_t)index]});
    return out;
}
// Whether `index` on the calibrate row asks for the T-pose (false: it takes the ask back).
inline bool TabAsksCalibration(const TabRow& row, int index) { return row.kind == TabRowKind::calibrate && index == 1; }

// The tab, top to bottom: moving, combat, the picture, the HUD, feel.
inline const std::vector<TabRow>& TabRows() {
    static const std::vector<TabRow> rows = {
        {TabRowKind::list, "Movement", "Forward", L"Walk toward", L"What forward on the left stick means.", "head",
         {{L"Where you look", "head"}, {L"Where the left hand points", "hand"}, {L"The stick's view", "camera"},
          {L"Where the body faces", "body"}}},
        {TabRowKind::list, "Movement", "SnapTurn", L"Turning",
         L"The right stick turns smoothly, or in steps (snap turns are easier on the stomach).", "false",
         {{L"Smooth", "false"}, {L"Snap 15\x00b0", "true", "15"}, {L"Snap 30\x00b0", "true", "30"},
          {L"Snap 45\x00b0", "true", "45"}, {L"Snap 60\x00b0", "true", "60"}, {L"Snap 90\x00b0", "true", "90"}},
         "SnapTurnDegrees", "30"},
        {TabRowKind::list, "Movement", "TurnSpeed", L"Smooth turn speed", L"How fast the right stick turns you, at full tilt.",
         "120",
         {{L"30\x00b0/s", "30"}, {L"60\x00b0/s", "60"}, {L"90\x00b0/s", "90"}, {L"120\x00b0/s", "120"},
          {L"150\x00b0/s", "150"}, {L"180\x00b0/s", "180"}, {L"210\x00b0/s", "210"}, {L"240\x00b0/s", "240"},
          {L"270\x00b0/s", "270"}, {L"300\x00b0/s", "300"}, {L"330\x00b0/s", "330"}, {L"360\x00b0/s", "360"}}},
        {TabRowKind::onOff, "Combat", "Enabled", L"Physical melee",
         L"Your swing is the attack: the weapon in your hand is the hitbox. Off: swings press the attack buttons.", "true"},
        {TabRowKind::list, "Combat", "KineticDamageScale", L"Damage", L"Every swing's damage, the difficulty knob: 50% hits for half.",
         "1.0",
         {{L"25%", "0.25"}, {L"50%", "0.5"}, {L"75%", "0.75"}, {L"100%", "1.0"}, {L"125%", "1.25"}, {L"150%", "1.5"},
          {L"175%", "1.75"}, {L"200%", "2.0"}}},
        {TabRowKind::onOff, "Combat", "Inertia", L"Weapon weight",
         L"Heavy weapons lag behind your hand and resist turning; two hands steer them better.", "true"},
        {TabRowKind::list, "FirstPerson", "HandRotOffsetX", L"Weapon angle",
         L"How the weapon sits in your hand: higher tips the blade forward, lower stands it back toward you.", "45",
         {{L"0\x00b0", "0"}, {L"15\x00b0", "15"}, {L"30\x00b0", "30"}, {L"45\x00b0", "45"}, {L"60\x00b0", "60"},
          {L"75\x00b0", "75"}, {L"90\x00b0", "90"}, {L"105\x00b0", "105"}, {L"120\x00b0", "120"}}},
        {TabRowKind::calibrate, "Body", "EyeHeightMm", L"Body calibration",
         // The help line shows about 125 characters on this page (flat, 2026-11-04); longer is cut off.
         L"Fits the Tarnished's arms and belt to you. Choose it, close the menu, then T-pose holding both triggers.",
         "0", {}},
        {TabRowKind::list, "VR", "WorldScale", L"World scale",
         L"Below 100% the world feels larger, above it smaller (it scales the distance between your eyes).", "1.0",
         {{L"50%", "0.5"}, {L"60%", "0.6"}, {L"70%", "0.7"}, {L"80%", "0.8"}, {L"90%", "0.9"}, {L"100%", "1.0"},
          {L"110%", "1.1"}, {L"120%", "1.2"}, {L"130%", "1.3"}, {L"140%", "1.4"}, {L"150%", "1.5"}, {L"160%", "1.6"},
          {L"170%", "1.7"}, {L"180%", "1.8"}, {L"190%", "1.9"}, {L"200%", "2.0"}}},
        {TabRowKind::list, "HUD", "Lock", L"HUD follows",
         L"Your body: the HUD stays put while you look around and follows when you turn or walk away. Your head: "
         L"welded to the view.",
         "body", {{L"Your body", "body"}, {L"Your head", "head"}, {L"Stays put", "world"}}},
        {TabRowKind::list, "HUD", "Distance", L"HUD distance", L"How far in front of you the HUD floats.", "2.0",
         {{L"1 m", "1.0"}, {L"1.25 m", "1.25"}, {L"1.5 m", "1.5"}, {L"1.75 m", "1.75"}, {L"2 m", "2.0"},
          {L"2.5 m", "2.5"}, {L"3 m", "3.0"}, {L"3.5 m", "3.5"}, {L"4 m", "4.0"}}},
        {TabRowKind::list, "Haptics", "RumbleScale", L"Rumble strength",
         L"How strongly the game's rumble plays on the controllers. 0% turns it off.", "1.0",
         {{L"0%", "0"}, {L"25%", "0.25"}, {L"50%", "0.5"}, {L"75%", "0.75"}, {L"100%", "1.0"}, {L"125%", "1.25"},
          {L"150%", "1.5"}, {L"175%", "1.75"}, {L"200%", "2.0"}}},
    };
    return rows;
}

} // namespace ervr
