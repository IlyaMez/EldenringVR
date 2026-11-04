#include "settings_tab_logic.h"
#include <cstdio>
#include <cstdlib>
#include <set>

using namespace ervr;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}
const TabRow& Row(const char* section, const char* key) {
    for (const TabRow& r : TabRows())
        if (launcher::text::sameText(r.section, section) && launcher::text::sameText(r.key, key))
            return r;
    std::fprintf(stderr, "no row %s.%s\n", section, key);
    std::exit(1);
}

int main() {
    // The page holds 16 rows and a list 32 choices; every row's default is one of its own choices.
    Check(TabRows().size() <= (size_t)kTabMaxRows, "the tab fits the game's page");
    std::set<std::string> keys;
    for (const TabRow& r : TabRows()) {
        Check(r.choices.size() <= (size_t)kTabMaxChoices - 1, "a list leaves room for the ini's own value");
        Check(keys.insert(std::string(r.section) + "." + r.key).second, "no key has two rows");
        const TabShown s = TabShow(r, "");
        Check(s.current == s.fallback, "an empty ERVR.ini shows every default");
        if (r.kind == TabRowKind::list)
            Check(s.labels.size() == r.choices.size(), "a default is always on its list");
    }

    // Switches read as ERVR reads them; a missing key is the default.
    {
        const TabRow& r = Row("Combat", "Inertia");
        Check(TabShow(r, "[Combat]\nInertia=false\n").current == 0 && TabShow(r, "[Combat]\nInertia=ON\n").current == 1 &&
                  TabShow(r, "[Combat]\nInertia=1\n").current == 1 && TabShow(r, "[Combat]\n").current == 1,
              "a switch reads as ERVR reads it");
        const auto w = TabWrites(r, TabShow(r, ""), 0);
        Check(w.size() == 1 && w[0].section == "Combat" && w[0].key == "Inertia" && w[0].value == "false", "off writes false");
    }

    // Numbers match by size, words without regard to case; a value off the list shows as itself and stays.
    {
        const TabRow& r = Row("Combat", "KineticDamageScale");
        const TabShown one = TabShow(r, "[Combat]\nKineticDamageScale=1\n");
        Check(r.choices[(size_t)one.current].value == std::string("1.0"), "1 is 100%");
        const TabShown odd = TabShow(r, "[Combat]\nKineticDamageScale=0.6\n");
        Check(odd.current == (int)r.choices.size() && odd.labels.back() == L"0.6 (ERVR.ini)", "0.6 shows as itself");
        const auto back = TabWrites(r, odd, odd.current);
        Check(back.size() == 1 && back[0].value == "0.6", "choosing it again writes it back unchanged");
        const auto half = TabWrites(r, odd, 1);
        Check(half.size() == 1 && half[0].key == "KineticDamageScale" && half[0].value == "0.5", "a choice writes its value");
        Check(TabShow(Row("HUD", "Lock"), "[HUD]\nLock=HEAD\n").current == 1, "words match without regard to case");
    }

    // Turning sets two keys; smooth is SnapTurn off whatever the step says.
    {
        const TabRow& r = Row("Movement", "SnapTurn");
        Check(TabShow(r, "[Movement]\nSnapTurn=false\nSnapTurnDegrees=45\n").current == 0, "snap off is smooth");
        const TabShown s = TabShow(r, "[Movement]\nSnapTurn=true\nSnapTurnDegrees=45\n");
        Check(s.current == 3, "snap on at 45 degrees");
        const auto w = TabWrites(r, s, 5);
        Check(w.size() == 2 && w[0].key == "SnapTurn" && w[0].value == "true" && w[1].key == "SnapTurnDegrees" &&
                  w[1].value == "90",
              "a snap choice writes both keys");
        const auto smooth = TabWrites(r, s, 0);
        Check(smooth.size() == 1 && smooth[0].value == "false", "smooth leaves the step as it was");
        const TabShown odd = TabShow(r, "[Movement]\nSnapTurn=true\nSnapTurnDegrees=20\n");
        Check(odd.current == 6 && odd.labels.back() == L"20 (ERVR.ini)", "an odd step shows as itself");
        const auto keep = TabWrites(r, odd, odd.current);
        Check(keep.size() == 2 && keep[0].value == "true" && keep[1].value == "20", "and writes back both values");
    }

    // Body calibration: what [Body] holds, then the T-pose again; it writes nothing, its second choice asks.
    {
        const TabRow& r = Row("Body", "EyeHeightMm");
        const TabShown none = TabShow(r, "");
        Check(none.labels.size() == 2 && none.labels[0] == L"Not yet" && none.labels[1] == L"Calibrate when the menu closes" &&
                  none.current == 0 && none.fallback == 0,
              "no calibration yet: not yet, calibrate");
        Check(TabShow(r, "[Body]\nCalibrationPrompt=false\n").labels[0] == L"Skipped", "a skipped one says so");
        const TabShown done = TabShow(r, "[Body]\nEyeHeightMm=1650\nArmLengthMm=618\nCalibrationPrompt=false\n");
        Check(done.labels[0] == L"1.76 m tall, arms 0.62 m" && done.labels[1] == L"Redo when the menu closes" &&
                  done.current == 0,
              "a calibration shows height (eyes / 0.936) and arms, as the launcher does");
        Check(TabShow(r, "[Body]\nEyeHeightMm=1650\nArmLengthMm=0\n").labels[0] == L"Not yet", "half a calibration is none");
        Check(TabWrites(r, done, 1).empty() && TabWrites(r, done, 0).empty(), "the row writes nothing itself");
        Check(TabAsksCalibration(r, 1) && !TabAsksCalibration(r, 0), "the second choice asks, the first takes it back");
        Check(!TabAsksCalibration(Row("Combat", "Inertia"), 1), "only that row asks");
    }

    // The edits land in the file the way the launcher's do: the key in place, every other byte kept.
    {
        const std::string ini = "; ERVR\r\n[Movement]\r\n; comment\r\nForward=head\r\nTurnSpeed=120\r\n";
        const TabRow& r = Row("Movement", "Forward");
        const TabShown s = TabShow(r, ini);
        Check(s.current == 0, "head is the first choice");
        std::string out = ini;
        for (const TabWrite& w : TabWrites(r, s, 1))
            out = launcher::text::iniSet(out, w.section, w.key, w.value);
        Check(out == "; ERVR\r\n[Movement]\r\n; comment\r\nForward=hand\r\nTurnSpeed=120\r\n", "only the value changed");
    }
    std::puts("settings tab: ok");
    return 0;
}
