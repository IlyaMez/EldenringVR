#include "weapon_presets.h"
#include "weapon_catalog.h"
#include "game_archive.h"
#include "icon_decode.h"
#include "weapon_preset_runtime.h"
#include "config.h"
#include <windows.h>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <fstream>
#include <thread>
#include <chrono>
#include <atomic>
using namespace ervr;
void Check(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
template <class T> void Put(std::vector<uint8_t>& b, size_t at, T v) {
    std::memcpy(b.data() + at, &v, sizeof(v));
}
int wmain(int argc, wchar_t** argv) {
    try {
        std::string why;
        WeaponPreset p;
        p.id = "test";
        p.name = "Test katana";
        p.weapon = 9060000;
        p.family = 13;
        p.tuning.Set(WeaponField::ContactOn, .8f);
        p.tuning.Set(WeaponField::ContactOff, .25f);
        p.tuning.Set(WeaponField::StanceHoldAnimation, 0.f);
        p.rules.push_back(
            {"stance", "Stance", WeaponEvent::Stance, WeaponAction::Heavy, WeaponCondition::TwoHand, 60, -1, true});
        Check(ValidateWeaponPreset(p, why), why.c_str());
        WeaponPreset copy;
        Check(ParseWeaponShare(WeaponShareCode(p), copy, why), why.c_str());
        Check(copy.name == p.name && copy.rules.size() == 1, "share round trip");
        Check(copy.tuning.Has(WeaponField::StanceHoldAnimation) &&
                  copy.tuning.Get(WeaponField::StanceHoldAnimation, 1.f) == 0.f,
              "hidden stance animation lost during share import");
        auto animationPreset = p;
        animationPreset.tuning.Set(WeaponField::StanceHoldAnimation, 1.f);
        Check(ParseWeaponPreset(WeaponPresetJson(animationPreset), copy, why) &&
                  copy.tuning.Get(WeaponField::StanceHoldAnimation, 0.f) == 1.f,
              "shown stance animation lost during file import");
        animationPreset.tuning.Set(WeaponField::StanceHoldAnimation, .5f);
        Check(!ParseWeaponPreset(WeaponPresetJson(animationPreset), copy, why),
              "fractional stance animation option accepted");
        animationPreset.tuning.Reset(WeaponField::StanceHoldAnimation);
        Check(ParseWeaponPreset(WeaponPresetJson(animationPreset), copy, why) &&
                  !copy.tuning.Has(WeaponField::StanceHoldAnimation),
              "existing presets should inherit stance animation");
        auto code = WeaponShareCode(p);
        code[code.size() - 1] = code.back() == '0' ? '1' : '0';
        Check(!ParseWeaponShare(code, copy, why), "corrupt share accepted");
        auto json = WeaponPresetJson(p);
        json.insert(1, "\"schemaVersion\":1,");
        Check(!ParseWeaponPreset(json, copy, why), "duplicate JSON key accepted");
        json = WeaponPresetJson(p);
        json.insert(1, "\"script\":\"x\",");
        Check(!ParseWeaponPreset(json, copy, why), "unknown field accepted");
        p.tuning.Set(WeaponField::ContactOff, 1);
        Check(!ValidateWeaponPreset(p, why), "inverted threshold accepted");
        p.tuning.Set(WeaponField::ContactOff, .25f);
        auto duplicate = p.rules[0];
        duplicate.id = "duplicate";
        p.rules.push_back(duplicate);
        Check(!ValidateWeaponPreset(p, why), "overlapping rules accepted");
        p.rules.pop_back();
        WeaponPresetStore store;
        store.presets.push_back(p);
        store.assignments.push_back({p.weapon, p.family, -1, p.id});
        auto resolved = ResolveWeaponPreset(store, p.weapon, p.family, 1);
        Check(DecideWeaponAction(resolved, WeaponEvent::Stance, {1, true, false}) == WeaponAction::Heavy,
              "two hand rule ignored");
        Check(DecideWeaponAction(resolved, WeaponEvent::Stance, {1, false, false}) == WeaponAction::Default,
              "context ignored");
        Check(ResolveWeaponPreset({}, p.weapon, p.family, 1).tuning.mask == 0, "no-preset defaults changed");
        auto family = p;
        family.id = "family";
        family.weapon = 0;
        family.tuning.Set(WeaponField::Reach, 1.3f);
        auto left = p;
        left.id = "left";
        left.tuning.Set(WeaponField::Reach, .9f);
        left.tuning.Set(WeaponField::StanceHoldAnimation, 1.f);
        store.presets.push_back(family);
        store.presets.push_back(left);
        store.assignments.push_back({0, p.family, -1, family.id});
        store.assignments.push_back({p.weapon, p.family, 0, left.id});
        Check(ResolveWeaponPreset(store, p.weapon, p.family, 0).fingerprint == CompileWeaponPreset(left).fingerprint,
              "left-hand precedence");
        Check(ResolveWeaponPreset(store, p.weapon, p.family, 1).fingerprint == CompileWeaponPreset(p).fingerprint,
              "wildcard captured left profile");
        Check(ResolveWeaponPreset(store, p.weapon, p.family, 0).tuning.Get(WeaponField::StanceHoldAnimation, 0.f) == 1.f &&
                  ResolveWeaponPreset(store, p.weapon, p.family, 1).tuning.Get(WeaponField::StanceHoldAnimation, 1.f) == 0.f,
              "stance animation did not follow the weapon's hand assignment");
        Check(ResolveWeaponPreset(store, p.weapon + 10000, p.family, 1).fingerprint ==
                  CompileWeaponPreset(family).fingerprint,
              "family fallback");
        Config baseline;
        Check(WeaponFieldBaseline(WeaponField::StanceHoldAnimation, baseline, p.family) == 1.f,
              "stance animation show default");
        baseline.stanceHoldAnimation = false;
        Check(WeaponFieldBaseline(WeaponField::StanceHoldAnimation, baseline, p.family) == 0.f,
              "stance animation global hide default");
        Check(WeaponFieldBaseline(WeaponField::Balance, baseline, 23) == .85f, "hammer geometry inheritance");
        baseline.weaponGeometry = "23:1.4:0.7";
        Check(WeaponFieldBaseline(WeaponField::Reach, baseline, 23) == 1.4f, "INI geometry inheritance");
        const auto file =
            std::filesystem::temp_directory_path() / ("ervr-workshop-test-" + NewWeaponPresetId()) / "library.json";
        Check(SaveWeaponPresetStore(file, store, 0, why), why.c_str());
        WeaponPresetStore loaded;
        Check(LoadWeaponPresetStore(file, loaded, why), why.c_str());
        Check(loaded.revision == 1 && loaded.presets.size() == 3, "store round trip");
        Check(!SaveWeaponPresetStore(file, loaded, 0, why), "stale editor overwrote library");
        std::filesystem::remove(file);
        std::filesystem::remove(file.wstring() + L".lock");
        std::filesystem::remove(file.parent_path());
        // Publication cannot expose half of a revision while the writer replaces snapshots.
        struct Sample {
            uint64_t revision = 0;
            std::array<uint64_t, 64> data{};
        };
        WeaponSnapshot<Sample> snapshots;
        std::atomic<bool> done = false, torn = false;
        std::thread reader([&] {
            while (!done.load())
                snapshots.Read([&](const Sample& sample) {
                    for (auto v : sample.data)
                        if (v != sample.revision)
                            torn.store(true);
                });
        });
        for (uint64_t i = 1; i <= 20000; ++i) {
            Sample sample;
            sample.revision = i;
            sample.data.fill(i);
            snapshots.Publish(sample);
        }
        done.store(true);
        reader.join();
        Check(!torn, "torn runtime snapshot");
        // Exercise real background reload, hand precedence, inactive imports and neutral-boundary application.
        const auto runtimeDir = std::filesystem::temp_directory_path() / ("ervr-runtime-test-" + NewWeaponPresetId());
        const auto runtimeFile = runtimeDir / "Presets" / "library.json";
        store.revision = 0;
        Check(SaveWeaponPresetStore(runtimeFile, store, 0, why), why.c_str());
        WeaponPresetEquipment(0, p.weapon, p.family);
        Check(!WeaponPresetApplyPending(false), "equipment without presets changed baseline behavior");
        WeaponPresetRuntimeStart(runtimeDir.wstring());
        struct StopRuntime {
            ~StopRuntime() { WeaponPresetRuntimeStop(); }
        } stopRuntime;
        WeaponPresetEquipment(0, p.weapon, p.family);
        WeaponPresetEquipment(1, p.weapon, p.family);
        auto waitFor = [&](auto predicate) {
            for (int i = 0; i < 200; ++i) {
                WeaponPresetApplyPending(true);
                if (predicate())
                    return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        };
        Check(waitFor([&] { return AppliedWeaponPreset(1).fingerprint == CompileWeaponPreset(p).fingerprint; }),
              "runtime library did not load");
        Check(AppliedWeaponPreset(0).fingerprint == CompileWeaponPreset(left).fingerprint, "runtime hand precedence");
        auto revised = store;
        revised.presets[0].revision++;
        revised.presets[0].tuning.Set(WeaponField::Reach, 1.7f);
        revised.presets[0].tuning.Set(WeaponField::StanceHoldAnimation, 1.f);
        const auto oldFingerprint = AppliedWeaponPreset(1).fingerprint;
        Check(SaveWeaponPresetStore(runtimeFile, revised, store.revision, why), why.c_str());
        store = revised;
        for (int i = 0; i < 70; ++i) {
            WeaponPresetApplyPending(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Check(AppliedWeaponPreset(1).fingerprint == oldFingerprint, "revision changed during an action");
        Check(AppliedWeaponPreset(1).tuning.Get(WeaponField::StanceHoldAnimation, 1.f) == 0.f,
              "stance animation changed during a held action");
        Check(waitFor([&] { return AppliedWeaponPreset(1).tuning.Get(WeaponField::Reach, 0) == 1.7f &&
                                  AppliedWeaponPreset(1).tuning.Get(WeaponField::StanceHoldAnimation, 0.f) == 1.f; }),
              "neutral revision not applied");
        WeaponPresetEquipment(1, p.weapon + 10000, p.family);
        Check(WeaponPresetApplyPending(false) && AppliedWeaponPreset(1).fingerprint == 0,
              "equipment swap did not cancel old preset");
        Check(WeaponPresetApplyPending(false), "equipment swap rearmed before neutral");
        Check(waitFor([&] { return AppliedWeaponPreset(1).fingerprint == CompileWeaponPreset(family).fingerprint; }),
              "new weapon family not applied");
        const auto beforeImport = AppliedWeaponPreset(1).fingerprint;
        auto inactive = family;
        inactive.id = "imported";
        inactive.revision = 42;
        store.presets.push_back(inactive);
        Check(SaveWeaponPresetStore(runtimeFile, store, store.revision, why), why.c_str());
        for (int i = 0; i < 70; ++i) {
            WeaponPresetApplyPending(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Check(AppliedWeaponPreset(1).fingerprint == beforeImport, "import activated itself");
        {
            std::ofstream corrupt(runtimeFile);
            corrupt << "broken";
        }
        for (int i = 0; i < 70; ++i) {
            WeaponPresetApplyPending(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Check(AppliedWeaponPreset(1).fingerprint == beforeImport, "corrupt library replaced last valid settings");
        WeaponPresetRuntimeStop();
        std::filesystem::remove(runtimeFile);
        std::filesystem::remove(runtimeFile.wstring() + L".lock");
        std::filesystem::remove(runtimeFile.parent_path());
        std::filesystem::remove(runtimeDir);
        std::vector<uint8_t> bad(64);
        std::vector<BinderEntry> binder;
        Check(!ReadWeaponBinder(bad, binder, why), "bad binder accepted");
        std::map<uint32_t, std::string> names;
        Check(!ReadWeaponNames(bad, names, why), "bad FMG accepted");
        // A real little-endian FMG v2 with one Unicode name at ID 1000000.
        std::vector<uint8_t> fmg(80);
        fmg[2] = 2;
        Put<uint32_t>(fmg, 12, 1);
        Put<uint32_t>(fmg, 16, 1);
        Put<uint64_t>(fmg, 24, 56);
        Put<uint32_t>(fmg, 44, 1000000);
        Put<uint32_t>(fmg, 48, 1000000);
        Put<uint64_t>(fmg, 56, 64);
        Put<uint16_t>(fmg, 64, 'A');
        Put<uint16_t>(fmg, 66, 0x03A9);
        Check(ReadWeaponNames(fmg, names, why) && names[1000000] == "A\xCE\xA9", "Unicode FMG parse");
        Put<uint32_t>(fmg, 48, 1000001);
        Check(!ReadWeaponNames(fmg, names, why), "FMG range overflow accepted");
        std::cout << "Pure preset / catalog checks passed\n";
        if (argc > 1) {
            WeaponCatalog catalog;
            Check(LoadWeaponCatalog(argv[1], "engus", catalog, why), why.c_str());
            std::cout << "Installed-game weapons: " << catalog.weapons.size() << "\n";
            size_t variants = 0;
            for (const auto& w : catalog.weapons) {
                variants += w.variants.size();
                std::cout << w.id << " | " << w.type << " | " << w.icon << " | " << w.name << " | variants "
                          << w.variants.size() << "\n";
            }
            std::cout << "Variant rows: " << variants << "\n";
            for (const auto& w : catalog.weapons) {
                if (argc < 3 && w.name != "Moonveil" && w.name != "Greatsword" && w.name != "Buckler" &&
                    w.name != "Rellana's Twin Blades")
                    continue;
                std::vector<uint8_t> tpf;
                Check(ArchiveReadItemIcon(w.icon, tpf, &why, argv[1]), why.c_str());
                std::vector<TpfTexture> textures;
                Check(TpfTextures(tpf.data(), tpf.size(), textures), "icon TPF");
                bool decoded = false;
                for (const auto& t : textures)
                    if (MenuIconId(t.name) == w.icon) {
                        std::vector<uint32_t> rgba;
                        uint32_t width = 0, height = 0;
                        decoded = DdsDecode(t.dds, t.ddsLen, rgba, width, height);
                        std::cout << "Icon verified: " << w.name << " " << width << "x" << height << "\n";
                    }
                Check(decoded, "real icon decode");
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
