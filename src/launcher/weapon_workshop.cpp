#include "weapon_workshop.hpp"
#include "weapon_workshop_help.hpp"
#include "system.hpp"
#include "weapon_catalog.h"
#include "weapon_presets.h"
#include "game_archive.h"
#include "icon_decode.h"
#include "config.h"
#include <d3d11.h>
#include <commdlg.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace launcher {
using namespace ervr;
namespace {
void Help(const char *meaning, const char *example = nullptr) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
        return;
    if (ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 29);
        ImGui::TextUnformatted(meaning);
        if (example && *example) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.82f, .69f, .40f, 1), "Example");
            ImGui::TextUnformatted(example);
        }
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
void Help(const workshop::Explanation &explanation) { Help(explanation.meaning, explanation.example); }
void HelpMarker(const char *meaning, const char *example) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    Help(meaning, example);
}
bool EditText(const char *label, std::string &text, size_t capacity = 128) {
    std::vector<char> buffer(capacity);
    strncpy_s(buffer.data(), buffer.size(), text.c_str(), _TRUNCATE);
    if (!ImGui::InputText(label, buffer.data(), buffer.size()))
        return false;
    text = buffer.data();
    return true;
}
std::string Lower(std::string text) {
    for (auto &c : text)
        if (c >= 'A' && c <= 'Z')
            c += (char)('a' - 'A');
    return text;
}
std::filesystem::path ChooseFile(bool save) {
    wchar_t path[32768] = L"weapon.ervrweapon.json";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFilter = L"Weapon preset (*.json)\0*.json\0All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = 32768;
    dialog.lpstrDefExt = L"json";
    dialog.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return (save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog)) ? std::filesystem::path(path)
                                                                          : std::filesystem::path{};
}
bool EffectiveValid(const WeaponPreset &preset, const Config &config, uint16_t family, std::string &why) {
    auto value = [&](WeaponField f) { return preset.tuning.Get(f, WeaponFieldBaseline(f, config, family)); };
    if (value(WeaponField::ContactOff) >= value(WeaponField::ContactOn))
        why = "Physical strikes: Speed considered at rest must be lower than Speed to start a strike.";
    else if (value(WeaponField::SwingOff) >= value(WeaponField::SwingOn))
        why = "Stance & swing detection: Speed to reset a swing must be lower than Speed to start a swing.";
    else if (value(WeaponField::Heavy) < value(WeaponField::SwingOn))
        why = "Stance & swing detection: Speed for a heavy attack must be at least Speed to start a swing.";
    else if (value(WeaponField::GrabMin) >= value(WeaponField::GrabMax))
        why = "Two-hand grip: Grab zone start must be lower than Grab zone end.";
    else
        return ValidateWeaponPreset(preset, why);
    return false;
}
} // namespace

struct WeaponWorkshop::State {
    ID3D11Device *device;
    std::wstring directory, iniPath;
    std::string language = "engus", error, status, search;
    WeaponCatalog catalog;
    Config config;
    WeaponPresetStore store;
    WeaponPreset draft, imported;
    uint32_t selected = 0, pendingSelection = 0;
    int familyFilter = 0, assignmentHand = -1;
    bool dirty = false, customOnly = false, equippedOnly = false, importReady = false, libraryOkay = true;
    std::array<char, 100000> paste{};
    uint32_t equipped[2]{};
    float tip[2]{}, speed[2]{};
    int liveEvent[2]{-1, -1}, liveAction[2]{};
    bool live = false;
    double lastLive = 0, lastConfig = 0;
    WeaponRuleContext testContext;
    int testEvent = 0;
    struct CpuIcon {
        uint32_t w = 0, h = 0;
        std::vector<uint32_t> pixels;
        std::string why;
    };
    struct Icon {
        ID3D11ShaderResourceView *view = nullptr;
        std::string why;
        bool requested = false;
    };
    std::map<int, Icon> icons;
    std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    bool stop = false, loading = false, ready = false;
    WeaponCatalog loaded;
    std::string loadError;
    std::deque<int> requests;
    std::map<int, CpuIcon> completed;

    explicit State(ID3D11Device *d) : device(d) {}
    ~State() {
        stopWorker();
        clearIcons();
    }
    void stopWorker() {
        {
            std::lock_guard lock(mutex);
            stop = true;
        }
        cv.notify_all();
        if (worker.joinable())
            worker.join();
    }
    void clearIcons() {
        for (auto &[id, icon] : icons) {
            (void)id;
            if (icon.view)
                icon.view->Release();
        }
        icons.clear();
    }
    std::filesystem::path libraryFile() const {
        return std::filesystem::path(directory) / L"ERVR" / L"Presets" / L"library.json";
    }
    const WeaponCatalogEntry *weapon(uint32_t id) const {
        for (const auto &w : catalog.weapons)
            if (w.id == id)
                return &w;
        return nullptr;
    }
    bool customized(const WeaponCatalogEntry &w) const {
        for (const auto &a : store.assignments)
            if (a.weapon == w.id || (!a.weapon && a.family == w.type))
                return true;
        return false;
    }
    bool isShield(const WeaponCatalogEntry &w) const {
        // Use the same configured type list and parsing as combat's IsShieldType.
        const auto &types = config.shieldWepTypes;
        for (size_t at = 0; at < types.size();) {
            auto end = types.find(',', at);
            if (end == std::string::npos)
                end = types.size();
            if (end > at && (uint16_t)std::atoi(types.substr(at, end - at).c_str()) == w.type)
                return true;
            at = end + 1;
        }
        return false;
    }
    void reloadLibrary() {
        WeaponPresetStore next;
        std::string why;
        if (LoadWeaponPresetStore(libraryFile(), next, why)) {
            store = std::move(next);
            libraryOkay = true;
            status = "Presets reloaded. Your unsaved changes are kept.";
        } else {
            libraryOkay = false;
            error = why;
        }
    }
    void start(const std::wstring &dir, const std::wstring &ini) {
        stopWorker();
        clearIcons();
        directory = dir;
        iniPath = ini;
        catalog = {};
        selected = 0;
        draft = {};
        dirty = false;
        error.clear();
        status.clear();
        config = Config{};
        config.Load(iniPath);
        reloadLibrary();
        status.clear();
        {
            std::lock_guard lock(mutex);
            stop = false;
            loading = true;
            ready = false;
            requests.clear();
            completed.clear();
            loaded = {};
            loadError.clear();
        }
        worker = std::thread([this, dir, lang = language] {
            WeaponCatalog result;
            std::string why;
            try {
                LoadWeaponCatalog(dir, lang, result, why);
            } catch (const std::exception &e) {
                why = e.what();
            }
            {
                std::lock_guard lock(mutex);
                loaded = std::move(result);
                loadError = why;
                ready = true;
                loading = false;
            }
            for (;;) {
                int id;
                {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [&] { return stop || !requests.empty(); });
                    if (stop)
                        return;
                    id = requests.front();
                    requests.pop_front();
                }
                CpuIcon out;
                try {
                    std::vector<uint8_t> bytes;
                    std::vector<TpfTexture> textures;
                    std::vector<uint32_t> rgba;
                    uint32_t w = 0, h = 0;
                    if (ArchiveReadItemIcon(id, bytes, &out.why, dir) &&
                        TpfTextures(bytes.data(), bytes.size(), textures)) {
                        for (const auto &texture : textures)
                            if (texture.dds && DdsDecode(texture.dds, texture.ddsLen, rgba, w, h)) {
                                ShrinkRgba(rgba, w, h, 128, out.pixels, out.w, out.h);
                                break;
                            }
                    }
                    if (out.pixels.empty() && out.why.empty())
                        out.why = "This icon could not be decoded from the installed game.";
                } catch (const std::exception &e) {
                    out.why = e.what();
                }
                {
                    std::lock_guard lock(mutex);
                    completed.emplace(id, std::move(out));
                }
            }
        });
    }
    void poll() {
        {
            std::lock_guard lock(mutex);
            if (ready) {
                catalog = std::move(loaded);
                error = loadError;
                ready = false;
                if (!catalog.weapons.empty())
                    select(catalog.weapons.front().id);
            }
            for (auto &[id, cpu] : completed) {
                auto &icon = icons[id];
                icon.why = cpu.why;
                if (cpu.pixels.empty())
                    continue;
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = cpu.w;
                desc.Height = cpu.h;
                desc.MipLevels = desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_IMMUTABLE;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA data{cpu.pixels.data(), cpu.w * 4, 0};
                ID3D11Texture2D *texture = nullptr;
                if (SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture))) {
                    if (FAILED(device->CreateShaderResourceView(texture, nullptr, &icon.view)))
                        icon.why = "Could not upload this icon.";
                    texture->Release();
                } else
                    icon.why = "Could not create this icon texture.";
            }
            completed.clear();
        }
        const double now = ImGui::GetTime();
        if (now - lastLive > .5) {
            lastLive = now;
            live = false;
            uint64_t tick = 0;
            uint32_t pid = 0;
            std::ifstream in(std::filesystem::path(directory) / L"ERVR" / L"weapon-live.txt");
            if (in >> pid >> tick) {
                const auto current = GetTickCount64();
                if (current >= tick && current - tick < 3000) {
                    live = true;
                    for (int h = 0; h < 2; ++h) {
                        uint16_t type = 0;
                        if (!(in >> equipped[h] >> type >> tip[h] >> speed[h] >> liveEvent[h] >> liveAction[h]))
                            live = false;
                    }
                }
            }
            if (!live)
                equipped[0] = equipped[1] = 0;
        }
        if (now - lastConfig > 2 && !iniPath.empty()) {
            lastConfig = now;
            Config fresh;
            fresh.Load(iniPath);
            config = std::move(fresh);
        }
    }
    void icon(int id, float size) {
        auto &img = icons[id];
        if (!img.requested) {
            img.requested = true;
            {
                std::lock_guard lock(mutex);
                requests.push_back(id);
            }
            cv.notify_one();
        }
        if (img.view)
            ImGui::Image((ImTextureID)(intptr_t)img.view, ImVec2(size, size));
        else {
            ImGui::Dummy(ImVec2(size, size));
            auto *draw = ImGui::GetWindowDrawList();
            const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            draw->AddRect(a, b, IM_COL32(96, 86, 60, 180), 4);
            draw->AddText(ImVec2(a.x + size * .35f, a.y + size * .3f), IM_COL32(176, 159, 112, 255),
                          img.why.empty() ? "..." : "?");
        }
        if (ImGui::IsItemHovered() && !img.why.empty())
            ImGui::SetTooltip("%s", img.why.c_str());
    }
    void fresh(bool family = false) {
        const auto *w = weapon(selected);
        if (!w)
            return;
        draft = {};
        draft.id = NewWeaponPresetId();
        draft.weapon = family ? 0 : w->id;
        draft.family = w->type;
        draft.name = family ? std::string(WeaponTypeName(w->type)) + " VR" : w->name + " VR";
        assignmentHand = -1;
        dirty = true;
    }
    void select(uint32_t id) {
        selected = id;
        assignmentHand = -1;
        dirty = false;
        draft = {};
        const auto *w = weapon(id);
        if (!w)
            return;
        if (const auto *active = AssignedWeaponPreset(store, w->id, w->type, 1))
            draft = *active;
        if (draft.id.empty())
            for (const auto &p : store.presets)
                if (p.weapon == w->id) {
                    draft = p;
                    break;
                }
        if (draft.id.empty()) {
            fresh();
            dirty = false;
        }
    }
    bool persist(WeaponPresetStore next, const char *message) {
        std::string why;
        if (!SaveWeaponPresetStore(libraryFile(), next, store.revision, why)) {
            error = why;
            return false;
        }
        store = std::move(next);
        error.clear();
        status = message;
        return true;
    }
    void save(bool activate) {
        const auto *w = weapon(selected);
        if (!w)
            return;
        std::string why;
        if (draft.family != w->type) {
            error = "Preset family does not match this weapon. Create a new preset for this installation.";
            return;
        }
        if (!EffectiveValid(draft, config, w->type, why)) {
            error = why;
            return;
        }
        auto next = store;
        WeaponPreset saved = draft;
        auto it =
            std::find_if(next.presets.begin(), next.presets.end(), [&](const auto &p) { return p.id == saved.id; });
        if (it != next.presets.end()) {
            if (it->revision == UINT32_MAX) {
                error = "Preset revision exhausted. Duplicate it to continue.";
                return;
            }
            saved.revision = it->revision + 1;
            *it = saved;
        } else
            next.presets.push_back(saved);
        if (activate) {
            std::erase_if(next.assignments, [&](const auto &a) {
                return a.weapon == saved.weapon && a.family == saved.family && a.hand == assignmentHand;
            });
            next.assignments.push_back({saved.weapon, saved.family, assignmentHand, saved.id});
        }
        if (persist(std::move(next),
                    activate ? "Saved. Release the controls and hold still in VR to apply your changes."
                             : "Changes saved. Any weapon already using this preset will also receive the changes.")) {
            draft = std::move(saved);
            dirty = false;
        }
    }
    void parseImport(std::string_view text) {
        std::string why;
        importReady = ParseWeaponShare(text, imported, why);
        if (!importReady)
            error = why;
        else
            error.clear();
    }
    void importModal(float scale) {
        const auto *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(.5f, .5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
                                            ImVec2(viewport->Size.x - 40 * scale, viewport->Size.y - 60 * scale));
        ImGui::SetNextWindowSize(ImVec2(650 * scale, 0), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal("Import weapon preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        ImGui::TextWrapped("Paste a shared preset below, or open a preset file. Importing adds a copy without changing "
                           "your current setup.");
        if (ImGui::InputTextMultiline("##paste", paste.data(), paste.size(), ImVec2(620 * scale, 110 * scale)))
            importReady = false;
        if (ImGui::Button("Review pasted settings"))
            parseImport(paste.data());
        Help("Read the pasted share code or JSON so you can review its settings before importing.");
        ImGui::SameLine();
        if (ImGui::Button("Open preset file")) {
            const auto file = ChooseFile(false);
            if (!file.empty()) {
                std::error_code ec;
                auto bytes = std::filesystem::file_size(file, ec);
                if (ec || bytes > 90000)
                    error = "Preset files must be smaller than 90 KB.";
                else {
                    std::ifstream in(file, std::ios::binary);
                    std::string text((size_t)bytes, '\0');
                    if (!in.read(text.data(), (std::streamsize)text.size()) ||
                        in.peek() != std::char_traits<char>::eof())
                        error = "The preset file changed while reading it. Retry.";
                    else
                        parseImport(text);
                }
            }
        }
        if (importReady) {
            ImGui::Separator();
            const auto *target = weapon(imported.weapon);
            const bool supported = imported.weapon
                                       ? target != nullptr
                                       : std::any_of(catalog.weapons.begin(), catalog.weapons.end(),
                                                     [&](const auto &w) { return w.type == imported.family; });
            ImGui::TextWrapped("%s  |  by %s", imported.name.c_str(),
                               imported.author.empty() ? "Anonymous" : imported.author.c_str());
            ImGui::TextWrapped("For: %s. %zu action rules.",
                               target ? target->name.c_str() : WeaponTypeName(imported.family), imported.rules.size());
            ImGui::BeginChild("review", ImVec2(620 * scale, 160 * scale), ImGuiChildFlags_Borders);
            for (const auto &f : WeaponFields())
                if (imported.tuning.Has(f.field)) {
                    const auto help = workshop::FieldHelp(f.field);
                    if (f.field == WeaponField::StanceHoldAnimation)
                        ImGui::Text("%s: %s", help.label,
                                    imported.tuning.Get(f.field, 0) != 0.f ? "Show while held" : "Hide while held");
                    else
                        ImGui::Text("%s: %.3g %s", help.label, imported.tuning.Get(f.field, 0), f.unit);
                    Help(help);
                }
            for (const auto &r : imported.rules)
                ImGui::TextWrapped("%s: %s -> %s (%s, priority %d)%s", r.name.c_str(),
                                   workshop::EventHelp(r.event).label, workshop::ActionHelp(r.action).label,
                                   workshop::ConditionLabel(r.condition), r.priority, r.enabled ? "" : " [disabled]");
            ImGui::EndChild();
            if (!supported)
                ImGui::TextWrapped("The target was not found in this game's catalog. This preset can be stored but "
                                   "cannot be activated here.");
            ImGui::BeginDisabled(!libraryOkay);
            if (ImGui::Button("Add to my presets")) {
                auto next = store;
                auto copy = imported;
                copy.id = NewWeaponPresetId();
                copy.revision = 1;
                next.presets.push_back(copy);
                if (persist(std::move(next),
                            "Preset added. Select it for the matching weapon, then Save & apply when you are ready.")) {
                    importReady = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            Help("Add an independent copy to your preset list. It will not change your current setup until you apply "
                 "it.");
            ImGui::EndDisabled();
        }
        if (!error.empty())
            ImGui::TextWrapped("%s", error.c_str());
        if (ImGui::Button("Close"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    void rules(const WeaponCatalogEntry &w) {
        const bool shield = isShield(w);
        ImGui::TextWrapped("Optional: choose what happens when you swing, grab, block or use a skill.");
        ImGui::TextDisabled("No rules = normal ERVR behavior.");
        HelpMarker("A rule changes an existing action when its trigger, condition and hand match. Global feature "
                   "settings still apply.",
                   "When you swing during a skill stance, choose a heavy follow-up only while two-handed.");
        int remove = -1;
        for (size_t i = 0; i < draft.rules.size(); ++i) {
            auto &r = draft.rules[i];
            ImGui::PushID((int)i);
            dirty |= ImGui::Checkbox("On", &r.enabled);
            Help("Turn this rule on or off without removing it.", "Turn it off to compare with normal behavior in VR.");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 5.5f);
            dirty |= EditText("Rule name", r.name, 97);
            Help("A name to help you recognize this rule.", "Heavy stance swings");
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * .68f);
            const bool eventOpen = ImGui::BeginCombo("When", workshop::EventHelp(r.event).label);
            Help(workshop::EventHelp(r.event));
            if (eventOpen) {
                for (int j = 0; j < (int)WeaponEvent::Count; ++j) {
                    if ((WeaponEvent)j == WeaponEvent::Guard && !shield)
                        continue;
                    const auto help = workshop::EventHelp((WeaponEvent)j);
                    if (ImGui::Selectable(help.label, (int)r.event == j)) {
                        r.event = (WeaponEvent)j;
                        if (!WeaponActionSupported(r.event, r.action))
                            r.action = WeaponAction::Default;
                        dirty = true;
                    }
                    Help(help);
                }
                ImGui::EndCombo();
            }
            if (r.event == WeaponEvent::Guard && !shield)
                ImGui::TextWrapped(
                    "This saved shield rule has no effect for this weapon. Choose another trigger or remove it.");
            const bool conditionOpen = ImGui::BeginCombo("Only if", workshop::ConditionLabel(r.condition));
            Help("Limit this rule to a situation. Any time applies regardless of grip or whether you are riding.",
                 "Two-handed applies only while holding the weapon with both hands.");
            if (conditionOpen) {
                for (int j = 0; j < (int)WeaponCondition::Count; ++j)
                    if (ImGui::Selectable(workshop::ConditionLabel((WeaponCondition)j), (int)r.condition == j)) {
                        r.condition = (WeaponCondition)j;
                        dirty = true;
                    }
                ImGui::EndCombo();
            }
            const bool actionOpen = ImGui::BeginCombo("Do this", workshop::ActionHelp(r.action).label);
            Help(workshop::ActionHelp(r.action));
            if (actionOpen) {
                for (int j = 0; j < (int)WeaponAction::Count; ++j)
                    if (WeaponActionSupported(r.event, (WeaponAction)j)) {
                        const auto help = workshop::ActionHelp((WeaponAction)j);
                        if (ImGui::Selectable(help.label, (int)r.action == j)) {
                            r.action = (WeaponAction)j;
                            dirty = true;
                        }
                        Help(help);
                    }
                ImGui::EndCombo();
            }
            int hand = r.hand + 1;
            dirty |= ImGui::Combo("Hand", &hand, "Either hand\0Left\0Right\0");
            Help("The hand that triggers this rule. This is separate from which hand uses the preset.",
                 "Right limits the rule to actions from the right-hand weapon.");
            r.hand = hand - 1;
            if (ImGui::TreeNode("If rules overlap")) {
                dirty |= ImGui::SliderInt("Priority", &r.priority, 0, 100);
                Help("The matching rule with the highest number wins. Rules that can match at the same time must have "
                     "different priorities.",
                     "A Two-handed rule at 60 beats an Any time rule at 50 when both match.");
                ImGui::TreePop();
            }
            ImGui::PopItemWidth();
            if (ImGui::SmallButton("Remove rule"))
                remove = (int)i;
            ImGui::Separator();
            ImGui::PopID();
        }
        if (remove >= 0) {
            draft.rules.erase(draft.rules.begin() + remove);
            dirty = true;
        }
        ImGui::BeginDisabled(draft.rules.size() >= 32);
        if (ImGui::Button("Add action rule")) {
            WeaponRule r;
            r.id = NewWeaponPresetId();
            r.name = "New rule";
            draft.rules.push_back(r);
            dirty = true;
        }
        Help("Add a rule, then choose When, Only if and Do this. You can have up to 32 rules.",
             "When: Swing during a skill stance. Only if: Two-handed. Do this: Heavy attack / follow-up.");
        ImGui::EndDisabled();
    }
    void tuning(const WeaponCatalogEntry &w) {
        ImGui::TextWrapped("Customize only what you want to change. Leave the rest on your defaults.");
        ImGui::TextDisabled("Hover a setting or value for help and an example.");
        struct Group {
            const char *key;
            const char *label;
            const char *description;
        };
        static const Group groups[] = {
            {"Fit", "Weapon alignment", "Adjust how the weapon sits in your hand."},
            {"Weight", "Weight & handling",
             "Change how quickly the weapon follows your hand. Requires weapon inertia (simulated weight)."},
            {"Contact", "Physical strikes", "Choose when continuous physical strikes begin and end."},
            {"Gesture", "Stance & swing detection",
             "Tune skill-stance swings and gated combat (one attack per detected swing)."},
            {"Grip", "Two-hand grip", "Adjust where your free hand can grab. Requires two-hand grab mode."},
            {"Skill", "Weapon skills", "Adjust skill holds, follow-up timing and projectile aiming."},
            {"Guard", "Shield blocking", "Adjust the left-hand shield's raised pose. Requires physical blocking."},
            {"Feedback", "Controller vibration", "Adjust combat vibration for this preset."}};
        for (const auto &group : groups) {
            if (!strcmp(group.key, "Guard") && !isShield(w))
                continue;
            if (ImGui::CollapsingHeader(group.label)) {
                ImGui::TextWrapped("%s", group.description);
                if (!ImGui::BeginTable(group.key, 2, ImGuiTableFlags_SizingStretchProp))
                    continue;
                ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, .57f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, .43f);
                for (const auto &f : WeaponFields())
                    if (!strcmp(group.key, f.group)) {
                        ImGui::PushID(f.key);
                        const auto help = workshop::FieldHelp(f.field);
                        bool override = draft.tuning.Has(f.field);
                        const float baseline = WeaponFieldBaseline(f.field, config, w.type);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (f.field == WeaponField::StanceHoldAnimation) {
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextUnformatted(help.label);
                            Help(help);
                            ImGui::TableNextColumn();
                            const char *choices[] = {baseline != 0.f ? "Use default (show)" : "Use default (hide)",
                                                     "Show while held", "Hide while held"};
                            int choice = override ? (draft.tuning.Get(f.field, baseline) != 0.f ? 1 : 2) : 0;
                            ImGui::SetNextItemWidth(-1);
                            if (ImGui::Combo("##animation", &choice, choices, IM_ARRAYSIZE(choices))) {
                                if (choice == 0)
                                    draft.tuning.Reset(f.field);
                                else
                                    draft.tuning.Set(f.field, choice == 1 ? 1.f : 0.f);
                                dirty = true;
                            }
                            Help(help);
                            ImGui::PopID();
                            continue;
                        }
                        if (ImGui::Checkbox(help.label, &override)) {
                            if (override)
                                draft.tuning.Set(f.field, std::clamp(baseline, f.min, f.max));
                            else
                                draft.tuning.Reset(f.field);
                            dirty = true;
                        }
                        std::string meaning = help.meaning;
                        char defaults[128];
                        sprintf_s(defaults, "\n\nYour default: %.3g %s. %s", baseline, f.unit,
                                  override ? "Uncheck to use your default again." : "Check to customize this setting.");
                        meaning += defaults;
                        Help(meaning.c_str(), help.example);
                        ImGui::TableNextColumn();
                        ImGui::BeginDisabled(!override);
                        float value = draft.tuning.Get(f.field, baseline);
                        ImGui::SetNextItemWidth(-1);
                        std::string format = "%.3g ";
                        format += f.unit;
                        if (ImGui::SliderFloat("##value", &value, f.min, f.max, format.c_str())) {
                            draft.tuning.Set(f.field, value);
                            dirty = true;
                        }
                        ImGui::EndDisabled();
                        Help(meaning.c_str(), help.example);
                        ImGui::PopID();
                    }
                ImGui::EndTable();
            }
        }
    }
    void test(const WeaponCatalogEntry &w) {
        const bool shield = isShield(w);
        if ((WeaponEvent)testEvent == WeaponEvent::Guard && !shield)
            testEvent = (int)WeaponEvent::Contact;
        ImGui::TextWrapped("See which action your rules would choose. This preview does not control the game.");
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * .68f);
        const bool triggerOpen = ImGui::BeginCombo("When", workshop::EventHelp((WeaponEvent)testEvent).label);
        Help(workshop::EventHelp((WeaponEvent)testEvent));
        if (triggerOpen) {
            for (int i = 0; i < (int)WeaponEvent::Count; ++i) {
                if ((WeaponEvent)i == WeaponEvent::Guard && !shield)
                    continue;
                if (ImGui::Selectable(workshop::EventHelp((WeaponEvent)i).label, testEvent == i))
                    testEvent = i;
            }
            ImGui::EndCombo();
        }
        ImGui::Combo("Hand##test", &testContext.hand, "Left\0Right\0");
        Help("Choose which hand to use for this preview.",
             "Select Right to check a rule restricted to the right hand.");
        ImGui::Checkbox("Two-handed", &testContext.twoHanded);
        Help("Preview as if you were holding the weapon with both hands.",
             "Check this to test a Two-handed condition.");
        ImGui::SameLine();
        ImGui::Checkbox("Riding Torrent", &testContext.mounted);
        Help("Preview as if you were on horseback.", "Leave this unchecked to test an On foot condition.");
        ImGui::PopItemWidth();
        std::string why;
        if (!EffectiveValid(draft, config, w.type, why))
            ImGui::TextWrapped("Draft needs correction: %s", why.c_str());
        else {
            int matched = -1;
            const auto compiled = CompileWeaponPreset(draft);
            const auto action = DecideWeaponAction(compiled, (WeaponEvent)testEvent, testContext, &matched);
            ImGui::TextWrapped("Would do: %s", workshop::ActionHelp(action).label);
            Help(workshop::ActionHelp(action));
            if (matched >= 0) {
                // Compilation sorts by priority and omits disabled entries.
                const auto &match = compiled.rules[matched];
                for (const auto &rule : draft.rules)
                    if (rule.enabled && rule.event == match.event && rule.action == match.action &&
                        rule.condition == match.condition && rule.hand == match.hand &&
                        rule.priority == match.priority) {
                        ImGui::TextWrapped("Rule: %s", rule.name.c_str());
                        break;
                    }
            } else
                ImGui::TextDisabled("No rule matched; the normal behavior applies.");
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Live readings from VR");
        HelpMarker("Shows measured movement and action decisions from the running mod. These readings do not confirm "
                   "hits or preview unsaved tuning.",
                   "Apply your changes, then compare tip speed here with your Speed to start a strike setting.");
        if (!live)
            ImGui::TextWrapped("Start VR with this version of ERVR to see equipped weapons and measured speeds here.");
        else
            for (int h = 0; h < 2; ++h) {
                const auto *held = weapon(equipped[h]);
                ImGui::TextWrapped("%s: %s  | tip %.2f m/s, hand %.2f m/s", h ? "Right" : "Left",
                                   held ? held->name.c_str() : "No recognized weapon", tip[h], speed[h]);
                if (liveEvent[h] >= 0)
                    ImGui::TextWrapped("%s -> %s", workshop::EventHelp((WeaponEvent)liveEvent[h]).label,
                                       workshop::ActionHelp((WeaponAction)liveAction[h]).label);
            }
        ImGui::TextWrapped("To try tuning changes, save and apply them, then test in VR.");
    }
    void editor(float scale) {
        const auto *w = weapon(selected);
        if (!w) {
            ImGui::TextWrapped("Select a weapon from the catalog.");
            return;
        }
        icon(w->icon, 56 * scale);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextWrapped("%s", w->name.c_str());
        ImGui::TextDisabled("%s", WeaponTypeName(w->type));
        Help("Upgrades and affinities share the same base weapon preset.",
             "A weapon and its upgraded version use the same settings here.");
        ImGui::EndGroup();
        std::string why;
        const bool valid = EffectiveValid(draft, config, w->type, why);
        const bool saved =
            std::any_of(store.presets.begin(), store.presets.end(), [&](const auto &p) { return p.id == draft.id; });
        ImGui::SetNextItemWidth(std::max(100 * scale, ImGui::GetContentRegionAvail().x - 145 * scale));
        ImGui::BeginDisabled(dirty);
        const bool presetOpen = ImGui::BeginCombo("##preset", draft.name.c_str());
        Help(dirty ? "Save or discard your changes before choosing another preset."
                   : "A preset is a named set of weapon settings. Choose one you have saved or imported.",
             "Keep a responsive preset for one weapon and a heavier-feeling version to compare.");
        if (presetOpen) {
            if (!saved)
                ImGui::TextDisabled("New preset - ready to customize");
            for (const auto &p : store.presets)
                if (p.weapon == w->id || (!p.weapon && p.family == w->type))
                    if (ImGui::Selectable((p.name + "##" + p.id).c_str(), draft.id == p.id)) {
                        draft = p;
                        dirty = false;
                    }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Preset options"))
            ImGui::OpenPopup("preset-options");
        Help("Create, copy, save, share or stop using a preset.");
        if (ImGui::BeginPopup("preset-options")) {
            if (ImGui::MenuItem("New for this weapon", nullptr, false, !dirty))
                fresh();
            Help("Start with your defaults for this weapon. Save or discard current changes first.",
                 "Create a separate setup for Moonveil.");
            if (ImGui::MenuItem("New for this weapon type", nullptr, false, !dirty))
                fresh(true);
            Help(
                "Start a preset for every weapon of this type. A preset applied to a specific weapon takes precedence.",
                "Use one setup for all katanas, then give Moonveil its own setup.");
            if (ImGui::MenuItem("Make a copy")) {
                draft.id = NewWeaponPresetId();
                draft.name += " copy";
                draft.revision = 1;
                dirty = true;
            }
            Help("Make an independent copy of these settings, including unsaved changes. The copy is not applied "
                 "automatically.",
                 "Try different handling without changing the preset you already use.");
            ImGui::Separator();
            if (ImGui::MenuItem("Save changes", nullptr, false, valid && libraryOkay))
                save(false);
            Help("Save without changing which weapons use this preset. Weapons already using it receive these changes "
                 "too.",
                 "A new preset stays unused until you choose Save & apply.");
            if (ImGui::MenuItem("Discard changes", nullptr, false, dirty)) {
                const auto current = std::find_if(store.presets.begin(), store.presets.end(),
                                                  [&](const auto &p) { return p.id == draft.id; });
                if (current != store.presets.end()) {
                    draft = *current;
                    dirty = false;
                } else {
                    fresh(!draft.weapon);
                    dirty = false;
                }
                status = "Unsaved changes discarded.";
            }
            Help("Restore the saved version of this preset. A new preset returns to your defaults.");
            const bool assigned = std::any_of(store.assignments.begin(), store.assignments.end(), [&](const auto &a) {
                return a.weapon == draft.weapon && a.family == draft.family && a.hand == assignmentHand;
            });
            if (ImGui::MenuItem("Remove preset from this target", nullptr, false, libraryOkay && assigned)) {
                auto next = store;
                std::erase_if(next.assignments, [&](const auto &a) {
                    return a.weapon == draft.weapon && a.family == draft.family && a.hand == assignmentHand;
                });
                persist(std::move(next),
                        "Preset removed from this target. Check Currently used for any remaining type or hand preset.");
            }
            Help("Remove the preset applied to the target and hand shown below. The saved preset stays in your "
                 "library. A broader preset may still apply.",
                 "Removing a right-hand weapon preset can reveal an Either hand or weapon-type preset.");
            ImGui::Separator();
            if (ImGui::MenuItem("Copy share code", nullptr, false, valid)) {
                ImGui::SetClipboardText(WeaponShareCode(draft).c_str());
                status = "Share code copied. It contains this preset's settings.";
            }
            Help("Copy these settings as text someone else can import. Includes unsaved changes.",
                 "Paste the code into a message to share your setup.");
            if (ImGui::MenuItem("Export preset file...", nullptr, false, valid)) {
                const auto file = ChooseFile(true);
                if (!file.empty()) {
                    std::error_code compareError;
                    if (std::filesystem::equivalent(file, libraryFile(), compareError))
                        error = "Choose a separate preset file instead of library.json.";
                    else {
                        std::ofstream out(file, std::ios::binary | std::ios::trunc);
                        out << WeaponPresetJson(draft);
                        out.close();
                        if (!out)
                            error = "Could not write the preset file.";
                        else
                            status = "Preset exported.";
                    }
                }
            }
            Help("Save these settings as a shareable JSON file, including unsaved changes.");
            ImGui::EndPopup();
        }

        ImGui::TextWrapped("Applies to: %s%s", draft.weapon ? "" : "All weapons of type: ",
                           draft.weapon ? w->name.c_str() : WeaponTypeName(draft.family));
        Help("Upgrades and affinities share this target. A weapon-specific preset takes precedence over a weapon-type "
             "preset.",
             "An upgraded Moonveil uses the same Moonveil preset.");
        int hand = assignmentHand + 1;
        ImGui::SetNextItemWidth(135 * scale);
        ImGui::Combo("##apply-hand", &hand, "Either hand\0Left hand\0Right hand\0");
        assignmentHand = hand - 1;
        Help("Choose which hand will use this preset when you save and apply. A hand-specific preset takes precedence "
             "over Either hand.",
             "Select Right hand to apply these settings only when this weapon is in your right hand.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!valid || !libraryOkay);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.42f, .34f, .18f, 1));
        if (ImGui::Button("Save & apply"))
            save(true);
        ImGui::PopStyleColor();
        Help(!libraryOkay ? "Reload presets to resolve the library error before saving."
             : !valid     ? why.c_str()
                          : "Save these settings and use them for the target and hand shown here. In VR, release the "
                            "controls and hold still to apply.",
             "Choose Right hand and Save & apply to use this setup for your right-hand weapon.");
        ImGui::EndDisabled();
        if (dirty) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, .76f, .38f, 1), "Unsaved changes");
        }
        if (!valid)
            ImGui::TextWrapped("%s", why.c_str());
        if (ImGui::TreeNode("Currently used & preset details")) {
            for (int h = 0; h < 2; ++h) {
                const auto *activePreset = AssignedWeaponPreset(store, w->id, w->type, h);
                ImGui::TextWrapped("%s: %s", h ? "Right hand" : "Left hand",
                                   activePreset ? activePreset->name.c_str() : "Your global settings");
            }
            HelpMarker("Shows the saved presets selected for this weapon. In VR, changes wait until you release "
                       "controls and hold still. Weapon + hand wins, then weapon + either hand, then weapon type + "
                       "hand, then weapon type + either hand.",
                       "A Moonveil right-hand preset takes precedence over an all-katanas preset.");
            ImGui::SetNextItemWidth(-85 * scale);
            dirty |= EditText("Name", draft.name, 97);
            Help("Give this setup a name you will recognize in the preset list.", "Moonveil - lighter feel");
            ImGui::SetNextItemWidth(-85 * scale);
            dirty |= EditText("Author", draft.author, 97);
            Help("Optional credit included when you share the preset.");
            ImGui::TreePop();
        }
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(.16f, .14f, .10f, 1));
        ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(.42f, .34f, .18f, 1));
        ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(.5f, .41f, .22f, 1));
        ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, ImVec4(.78f, .64f, .34f, 1));
        if (ImGui::BeginTabBar("preset-sections")) {
            if (ImGui::BeginTabItem("Feel & alignment")) {
                ImGui::BeginChild("tuning-content", ImVec2(0, 0));
                tuning(*w);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Action rules")) {
                ImGui::BeginChild("rules-content", ImVec2(0, 0));
                rules(*w);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Try rules & live VR")) {
                ImGui::BeginChild("test-content", ImVec2(0, 0));
                test(*w);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::PopStyleColor(4);
    }
    void draw(const std::wstring &dir, const std::wstring &ini, ImVec2 origin, ImVec2 size, float scale) {
        ImGui::SetCursorScreenPos(origin);
        ImGui::BeginChild("weapon-workshop", size);
        if (dir.empty()) {
            ImGui::TextWrapped("Choose your ELDEN RING installation on the Play tab to load its weapons and icons.");
            ImGui::EndChild();
            return;
        }
        if (dir != directory)
            start(dir, ini);
        iniPath = ini;
        poll();
        ImGui::TextUnformatted("WEAPON WORKSHOP");
        ImGui::SameLine();
        ImGui::TextDisabled("%zu weapons  |  %s", catalog.weapons.size(), live ? "VR connected" : "VR not connected");
        Help("You can edit and save presets without VR running. Connect VR to see equipped weapons and live movement "
             "readings.");
        ImGui::TextWrapped(
            "Choose a weapon, adjust how it feels, then save and apply. Hover controls or (?) for help.");
        if (ImGui::Button("Import preset")) {
            paste[0] = 0;
            importReady = false;
            ImGui::OpenPopup("Import weapon preset");
        }
        Help("Add a preset from a shared code or file. You can review it before adding it.");
        ImGui::SameLine();
        if (ImGui::Button("Reload presets"))
            reloadLibrary();
        Help("Read your saved presets again, keeping your current unsaved changes.",
             "Use this after another launcher window saves a preset.");
        if (!error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, .55f, .4f, 1));
            ImGui::TextWrapped("%s", error.c_str());
            ImGui::PopStyleColor();
        }
        if (!status.empty())
            ImGui::TextWrapped("%s", status.c_str());
        bool isLoading;
        {
            std::lock_guard lock(mutex);
            isLoading = loading;
        }
        if (isLoading)
            ImGui::TextWrapped("Reading weapon definitions and names from your installed game...");
        if (!isLoading && catalog.weapons.empty() && ImGui::Button("Try loading weapons again"))
            start(dir, ini);
        const float width = std::clamp(size.x * .32f, 240 * scale, 350 * scale);
        ImGui::BeginChild("catalog", ImVec2(width, 0), ImGuiChildFlags_Borders);
        ImGui::SetNextItemWidth(-1);
        EditText("##search", search);
        Help("Search the installed game's English weapon names, or enter a weapon ID.",
             "Try Moonveil, Greatsword or Buckler.");
        if (search.empty() && !ImGui::IsItemActive()) {
            auto pos = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + 8 * scale, pos.y + 8 * scale),
                                                ImGui::GetColorU32(ImGuiCol_TextDisabled), "Search weapons or ID...");
        }
        std::set<uint16_t> families;
        for (const auto &w : catalog.weapons)
            families.insert(w.type);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##family", familyFilter ? WeaponTypeName((uint16_t)familyFilter) : "All weapon types")) {
            if (ImGui::Selectable("All weapon types", !familyFilter))
                familyFilter = 0;
            for (auto type : families)
                if (ImGui::Selectable(WeaponTypeName(type), familyFilter == type))
                    familyFilter = type;
            ImGui::EndCombo();
        }
        ImGui::Checkbox("Has preset", &customOnly);
        Help("Show weapons with a preset applied to the weapon or its type. Saved but unused presets do not count.",
             "Apply a katana-type preset and every katana appears in this filter.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!live && !equippedOnly);
        ImGui::Checkbox("Equipped", &equippedOnly);
        ImGui::EndDisabled();
        Help("Show the weapons currently equipped in VR. Requires a running, connected ERVR session.");
        const auto query = Lower(search);
        std::vector<const WeaponCatalogEntry *> filtered;
        for (const auto &w : catalog.weapons)
            if ((!familyFilter || w.type == familyFilter) && (!customOnly || customized(w)) &&
                (!equippedOnly || w.id == equipped[0] || w.id == equipped[1]) &&
                (query.empty() || Lower(w.name).find(query) != std::string::npos ||
                 std::to_string(w.id).find(query) != std::string::npos))
                filtered.push_back(&w);
        std::stable_sort(filtered.begin(), filtered.end(), [&](auto a, auto b) {
            return (a->id == equipped[0] || a->id == equipped[1]) > (b->id == equipped[0] || b->id == equipped[1]);
        });
        ImGui::TextDisabled("%zu shown", filtered.size());
        ImGui::BeginChild("weapon-list", ImVec2(0, 0));
        const float row = 66 * scale;
        ImGuiListClipper clipper;
        clipper.Begin((int)filtered.size(), row);
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto &w = *filtered[i];
                ImGui::PushID((int)w.id);
                const auto pos = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##weapon", selected == w.id, 0,
                                      ImVec2(0, row - ImGui::GetStyle().ItemSpacing.y)) &&
                    selected != w.id) {
                    if (dirty) {
                        pendingSelection = w.id;
                    } else
                        select(w.id);
                }
                ImGui::SetCursorScreenPos(ImVec2(pos.x + 4 * scale, pos.y + 4 * scale));
                icon(w.icon, 52 * scale);
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::PushTextWrapPos(pos.x + width - 25 * scale);
                ImGui::TextUnformatted(w.name.c_str());
                ImGui::PopTextWrapPos();
                ImGui::TextDisabled("%s%s", WeaponTypeName(w.type), customized(w) ? " | Preset" : "");
                ImGui::EndGroup();
                ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + row - ImGui::GetStyle().ItemSpacing.y));
                ImGui::Dummy(ImVec2(0, 0));
                ImGui::PopID();
            }
        if (filtered.empty())
            ImGui::TextWrapped(equippedOnly && !live ? "No live VR equipment data yet." : "No matching weapons.");
        if (filtered.empty() && (customOnly || equippedOnly || familyFilter || !search.empty()) &&
            ImGui::Button("Clear filters")) {
            customOnly = equippedOnly = false;
            familyFilter = 0;
            search.clear();
        }
        ImGui::EndChild();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("weapon-editor", ImVec2(0, 0), ImGuiChildFlags_Borders);
        editor(scale);
        ImGui::EndChild();
        if (pendingSelection)
            ImGui::OpenPopup("Unsaved changes");
        if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("This preset has unsaved changes.");
            std::string why;
            const auto *current = weapon(selected);
            ImGui::BeginDisabled(!libraryOkay || !current || !EffectiveValid(draft, config, current->type, why));
            if (ImGui::Button("Save and switch")) {
                save(false);
                if (!dirty) {
                    select(pendingSelection);
                    pendingSelection = 0;
                    ImGui::CloseCurrentPopup();
                }
            }
            Help("Save before switching weapons. Weapons already using this preset receive the changes; a new preset "
                 "stays unused.");
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Keep editing")) {
                pendingSelection = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard and switch")) {
                select(pendingSelection);
                pendingSelection = 0;
                ImGui::CloseCurrentPopup();
            }
            if (!why.empty())
                ImGui::TextWrapped("%s", why.c_str());
            if (!error.empty())
                ImGui::TextWrapped("%s", error.c_str());
            ImGui::EndPopup();
        }
        importModal(scale);
        ImGui::EndChild();
    }
};
WeaponWorkshop::WeaponWorkshop(ID3D11Device *device) : state_(std::make_unique<State>(device)) {}
WeaponWorkshop::~WeaponWorkshop() = default;
void WeaponWorkshop::draw(const std::wstring &dir, const std::wstring &ini, ImVec2 origin, ImVec2 size, float scale) {
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(.82f, .69f, .40f, 1));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(.28f, .23f, .13f, 1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(.25f, .22f, .15f, 1));
    state_->draw(dir, ini, origin, size, scale);
    ImGui::PopStyleColor(3);
}
} // namespace launcher
