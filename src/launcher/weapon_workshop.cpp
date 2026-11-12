#include "weapon_workshop.hpp"
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
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace launcher {
using namespace ervr;
namespace {
bool EditText(const char* label, std::string& text, size_t capacity = 128) {
    std::vector<char> buffer(capacity);
    strncpy_s(buffer.data(), buffer.size(), text.c_str(), _TRUNCATE);
    if (!ImGui::InputText(label, buffer.data(), buffer.size()))
        return false;
    text = buffer.data();
    return true;
}
std::string Lower(std::string text) {
    for (auto& c : text)
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
bool EffectiveValid(const WeaponPreset& preset, const Config& config, uint16_t family, std::string& why) {
    if (!ValidateWeaponPreset(preset, why))
        return false;
    auto value = [&](WeaponField f) { return preset.tuning.Get(f, WeaponFieldBaseline(f, config, family)); };
    if (value(WeaponField::ContactOff) >= value(WeaponField::ContactOn) ||
        value(WeaponField::SwingOff) >= value(WeaponField::SwingOn) ||
        value(WeaponField::Heavy) < value(WeaponField::SwingOn) ||
        value(WeaponField::GrabMin) >= value(WeaponField::GrabMax)) {
        why = "An override conflicts with an inherited threshold. End must be below begin; heavy must be at least "
              "begin; grip start must be below end.";
        return false;
    }
    return true;
}
} // namespace

struct WeaponWorkshop::State {
    ID3D11Device* device;
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
        ID3D11ShaderResourceView* view = nullptr;
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

    explicit State(ID3D11Device* d) : device(d) {}
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
        for (auto& [id, icon] : icons) {
            (void)id;
            if (icon.view)
                icon.view->Release();
        }
        icons.clear();
    }
    std::filesystem::path libraryFile() const {
        return std::filesystem::path(directory) / L"ERVR" / L"Presets" / L"library.json";
    }
    const WeaponCatalogEntry* weapon(uint32_t id) const {
        for (const auto& w : catalog.weapons)
            if (w.id == id)
                return &w;
        return nullptr;
    }
    bool customized(const WeaponCatalogEntry& w) const {
        for (const auto& a : store.assignments)
            if (a.weapon == w.id || (!a.weapon && a.family == w.type))
                return true;
        return false;
    }
    void reloadLibrary() {
        WeaponPresetStore next;
        std::string why;
        if (LoadWeaponPresetStore(libraryFile(), next, why)) {
            store = std::move(next);
            libraryOkay = true;
            status = "Library reloaded. Your current draft is preserved.";
        } else {
            libraryOkay = false;
            error = why;
        }
    }
    void start(const std::wstring& dir, const std::wstring& ini) {
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
            } catch (const std::exception& e) {
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
                        for (const auto& texture : textures)
                            if (texture.dds && DdsDecode(texture.dds, texture.ddsLen, rgba, w, h)) {
                                ShrinkRgba(rgba, w, h, 128, out.pixels, out.w, out.h);
                                break;
                            }
                    }
                    if (out.pixels.empty() && out.why.empty())
                        out.why = "This icon could not be decoded from the installed game.";
                } catch (const std::exception& e) {
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
            for (auto& [id, cpu] : completed) {
                auto& icon = icons[id];
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
                ID3D11Texture2D* texture = nullptr;
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
        auto& img = icons[id];
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
            auto* draw = ImGui::GetWindowDrawList();
            const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            draw->AddRect(a, b, IM_COL32(96, 86, 60, 180), 4);
            draw->AddText(ImVec2(a.x + size * .35f, a.y + size * .3f), IM_COL32(176, 159, 112, 255),
                          img.why.empty() ? "..." : "?");
        }
        if (ImGui::IsItemHovered() && !img.why.empty())
            ImGui::SetTooltip("%s", img.why.c_str());
    }
    void fresh(bool family = false) {
        const auto* w = weapon(selected);
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
        dirty = false;
        draft = {};
        const auto* w = weapon(id);
        if (!w)
            return;
        if (const auto* active = AssignedWeaponPreset(store,w->id,w->type,1)) draft = *active;
        if (draft.id.empty())
            for (const auto& p : store.presets)
                if (p.weapon == w->id) {
                    draft = p;
                    break;
                }
        if (draft.id.empty()) {
            fresh();
            dirty = false;
        }
    }
    bool persist(WeaponPresetStore next, const char* message) {
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
        const auto* w = weapon(selected);
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
            std::find_if(next.presets.begin(), next.presets.end(), [&](const auto& p) { return p.id == saved.id; });
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
            std::erase_if(next.assignments, [&](const auto& a) {
                return a.weapon == saved.weapon && a.family == saved.family && a.hand == assignmentHand;
            });
            next.assignments.push_back({saved.weapon, saved.family, assignmentHand, saved.id});
        }
        if (persist(std::move(next),
                    activate ? "Saved and assigned. In VR, release controls and hold still to apply between actions."
                             : "Preset saved. Existing assignments to this preset also receive the revision.")) {
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
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(.5f, .5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
                                           ImVec2(viewport->Size.x - 40 * scale, viewport->Size.y - 60 * scale));
        ImGui::SetNextWindowSize(ImVec2(650 * scale, 0), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal("Import weapon preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        ImGui::TextWrapped(
            "Paste a share code or preset JSON. Review it here, then save an inactive copy to your library.");
        if (ImGui::InputTextMultiline("##paste", paste.data(), paste.size(), ImVec2(620 * scale, 110 * scale)))
            importReady = false;
        if (ImGui::Button("Review pasted settings"))
            parseImport(paste.data());
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
            const auto* target = weapon(imported.weapon);
            const bool supported = imported.weapon
                                       ? target != nullptr
                                       : std::any_of(catalog.weapons.begin(), catalog.weapons.end(),
                                                     [&](const auto& w) { return w.type == imported.family; });
            ImGui::TextWrapped("%s  |  by %s", imported.name.c_str(),
                               imported.author.empty() ? "Anonymous" : imported.author.c_str());
            ImGui::TextWrapped("Target: %s (%u). %zu rules.",
                               target ? target->name.c_str() : WeaponTypeName(imported.family), imported.weapon,
                               imported.rules.size());
            ImGui::BeginChild("review", ImVec2(620 * scale, 160 * scale), ImGuiChildFlags_Borders);
            for (const auto& f : WeaponFields())
                if (imported.tuning.Has(f.field))
                    ImGui::Text("%s: %.3g %s", f.label, imported.tuning.Get(f.field, 0), f.unit);
            for (const auto& r : imported.rules)
                ImGui::TextWrapped("%s: %s -> %s (%s, priority %d)%s", r.name.c_str(), WeaponEventName(r.event),
                                   WeaponActionName(r.action), WeaponConditionName(r.condition), r.priority,
                                   r.enabled ? "" : " [disabled]");
            ImGui::EndChild();
            if (!supported)
                ImGui::TextWrapped("The target was not found in this game's catalog. This preset can be stored but "
                                   "cannot be activated here.");
            ImGui::BeginDisabled(!libraryOkay);
            if (ImGui::Button("Save inactive copy")) {
                auto next = store;
                auto copy = imported;
                copy.id = NewWeaponPresetId();
                copy.revision = 1;
                next.presets.push_back(copy);
                if (persist(std::move(next),
                            "Imported as an inactive copy. Select it from the matching weapon's preset list.")) {
                    importReady = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndDisabled();
        }
        if (!error.empty())
            ImGui::TextWrapped("%s", error.c_str());
        if (ImGui::Button("Close"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    void rules() {
        ImGui::TextWrapped("Triggers use ERVR's existing detectors and configured skill control. Highest matching "
                           "priority wins. Equal-priority overlaps must be resolved before saving.");
        ImGui::TextWrapped("Shield-pose rules use the left-hand shield detector. Effect skill rules keep the existing "
                           "two-handed recording requirements.");
        ImGui::TextWrapped("Light / heavy selects a right-hand or two-handed attack. An independent left-hand swing "
                           "keeps its left-hand attack.");
        int remove = -1;
        for (size_t i = 0; i < draft.rules.size(); ++i) {
            auto& r = draft.rules[i];
            ImGui::PushID((int)i);
            dirty |= ImGui::Checkbox("##enabled", &r.enabled);
            ImGui::SameLine();
            dirty |= EditText("Rule name", r.name, 97);
            if (ImGui::BeginCombo("When", WeaponEventName(r.event))) {
                for (int j = 0; j < (int)WeaponEvent::Count; ++j)
                    if (ImGui::Selectable(WeaponEventName((WeaponEvent)j), (int)r.event == j)) {
                        r.event = (WeaponEvent)j;
                        if (!WeaponActionSupported(r.event, r.action))
                            r.action = WeaponAction::Default;
                        dirty = true;
                    }
                ImGui::EndCombo();
            }
            if (ImGui::BeginCombo("Condition", WeaponConditionName(r.condition))) {
                for (int j = 0; j < (int)WeaponCondition::Count; ++j)
                    if (ImGui::Selectable(WeaponConditionName((WeaponCondition)j), (int)r.condition == j)) {
                        r.condition = (WeaponCondition)j;
                        dirty = true;
                    }
                ImGui::EndCombo();
            }
            if (ImGui::BeginCombo("Action", WeaponActionName(r.action))) {
                for (int j = 0; j < (int)WeaponAction::Count; ++j)
                    if (WeaponActionSupported(r.event, (WeaponAction)j) &&
                        ImGui::Selectable(WeaponActionName((WeaponAction)j), (int)r.action == j)) {
                        r.action = (WeaponAction)j;
                        dirty = true;
                    }
                ImGui::EndCombo();
            }
            int hand = r.hand + 1;
            dirty |= ImGui::Combo("Hand", &hand, "Either hand\0Left\0Right\0");
            r.hand = hand - 1;
            dirty |= ImGui::SliderInt("Priority", &r.priority, 0, 100);
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
        ImGui::EndDisabled();
    }
    void tuning(const WeaponCatalogEntry& w) {
        ImGui::TextWrapped("Global feature switches still apply: enable physical combat, inertia, support grip, or "
                           "shield blocking in Options / ERVR.ini as needed.");
        ImGui::TextWrapped("Check a setting to override it. Unchecked settings inherit your ERVR options; fit angles "
                           "and grip offsets are corrections to the global calibration.");
        for (const char* group : {"Fit", "Weight", "Contact", "Gesture", "Grip", "Skill", "Guard", "Feedback"})
            if (ImGui::CollapsingHeader(group, !strcmp(group, "Fit") ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                for (const auto& f : WeaponFields())
                    if (!strcmp(group, f.group)) {
                        ImGui::PushID(f.key);
                        bool override = draft.tuning.Has(f.field);
                        const float baseline = WeaponFieldBaseline(f.field, config, w.type);
                        if (ImGui::Checkbox("##override", &override)) {
                            if (override)
                                draft.tuning.Set(f.field, std::clamp(baseline, f.min, f.max));
                            else
                                draft.tuning.Reset(f.field);
                            dirty = true;
                        }
                        ImGui::SameLine();
                        ImGui::BeginDisabled(!override);
                        float value = draft.tuning.Get(f.field, baseline);
                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * .51f);
                        std::string format = "%.3g ";
                        format += f.unit;
                        if (ImGui::SliderFloat(f.label, &value, f.min, f.max, format.c_str())) {
                            draft.tuning.Set(f.field, value);
                            dirty = true;
                        }
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip("Inherited: %.3g %s. %s", baseline, f.unit,
                                              override ? "Uncheck to inherit." : "Check to customize.");
                        ImGui::PopID();
                    }
            }
    }
    void test(const WeaponCatalogEntry& w) {
        ImGui::TextWrapped("Rule preview evaluates this draft without sending input to the game. Detector tuning is "
                           "tested in VR after saving and activating.");
        if (ImGui::BeginCombo("Trigger", WeaponEventName((WeaponEvent)testEvent))) {
            for (int i = 0; i < (int)WeaponEvent::Count; ++i)
                if (ImGui::Selectable(WeaponEventName((WeaponEvent)i), testEvent == i))
                    testEvent = i;
            ImGui::EndCombo();
        }
        ImGui::Combo("Hand##test", &testContext.hand, "Left\0Right\0");
        ImGui::Checkbox("Two-handed", &testContext.twoHanded);
        ImGui::SameLine();
        ImGui::Checkbox("Mounted", &testContext.mounted);
        std::string why;
        if (!EffectiveValid(draft, config, w.type, why))
            ImGui::TextWrapped("Draft needs correction: %s", why.c_str());
        else {
            int matched = -1;
            const auto compiled = CompileWeaponPreset(draft);
            const auto action = DecideWeaponAction(compiled, (WeaponEvent)testEvent, testContext, &matched);
            ImGui::TextWrapped("Result: %s", WeaponActionName(action));
            if (matched >= 0)
                ImGui::Text("Matched priority: %d", compiled.rules[matched].priority);
            else
                ImGui::TextDisabled("No rule matched; the normal behavior applies.");
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Live VR diagnostics");
        if (!live)
            ImGui::TextWrapped("Start VR with this version of ERVR to see equipped weapons and measured speeds here.");
        else
            for (int h = 0; h < 2; ++h) {
                const auto* held = weapon(equipped[h]);
                ImGui::TextWrapped("%s: %s  | tip %.2f m/s, hand %.2f m/s", h ? "Right" : "Left",
                                   held ? held->name.c_str() : "No recognized weapon", tip[h], speed[h]);
                if (liveEvent[h] >= 0)
                    ImGui::TextWrapped("%s -> %s", WeaponEventName((WeaponEvent)liveEvent[h]),
                                       WeaponActionName((WeaponAction)liveAction[h]));
            }
        ImGui::TextWrapped("Live readings show detector input and rule decisions, not confirmed hits. The game still owns "
                           "stamina, damage, casts and animation availability.");
    }
    void editor(float scale) {
        const auto* w = weapon(selected);
        if (!w) {
            ImGui::TextWrapped("Select a weapon from the catalog.");
            return;
        }
        icon(w->icon, 72 * scale);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextWrapped("%s", w->name.c_str());
        ImGui::TextDisabled("%s  |  ID %u", WeaponTypeName(w->type), w->id);
        ImGui::TextDisabled("Game weight %.1f  |  %zu affinity rows", w->weight, w->variants.size());
        ImGui::EndGroup();
        ImGui::BeginDisabled(dirty);
        if (ImGui::BeginCombo("Preset", draft.name.c_str())) {
            for (const auto& p : store.presets)
                if (p.weapon == w->id || (!p.weapon && p.family == w->type))
                    if (ImGui::Selectable((p.name + "##" + p.id).c_str(), draft.id == p.id)) {
                        draft = p;
                        dirty = false;
                    }
            ImGui::EndCombo();
        }
        if (ImGui::Button("New weapon preset"))
            fresh();
        ImGui::SameLine();
        if (ImGui::Button("New family preset"))
            fresh(true);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Duplicate")) {
            draft.id = NewWeaponPresetId();
            draft.name += " copy";
            draft.revision = 1;
            dirty = true;
        }
        if (ImGui::BeginTable("identity", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-55 * scale);
            dirty |= EditText("Name", draft.name, 97);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-55 * scale);
            dirty |= EditText("Author", draft.author, 97);
            ImGui::EndTable();
        }
        ImGui::TextWrapped("Scope: %s. Reinforcement and affinities share the base weapon's preset.",
                           draft.weapon ? w->name.c_str() : WeaponTypeName(draft.family));
        int hand = assignmentHand + 1;
        ImGui::Combo("Assign to", &hand, "Either hand\0Left hand\0Right hand\0");
        assignmentHand = hand - 1;
        for (int h = 0; h < 2; ++h) {
            const auto* activePreset = AssignedWeaponPreset(store,w->id,w->type,h);
            if (h)
                ImGui::SameLine();
            ImGui::TextDisabled("%s: %s", h ? "Right" : "Left",
                                activePreset ? activePreset->name.c_str() : "Global settings");
        }
        std::string why;
        const bool valid = EffectiveValid(draft, config, w->type, why);
        ImGui::BeginDisabled(!valid || !libraryOkay);
        if (ImGui::Button("Save & activate"))
            save(true);
        ImGui::SameLine();
        if (ImGui::Button("Save only"))
            save(false);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Revert draft")) {
            select(selected);
            status = "Draft restored from the saved library.";
        }
        ImGui::BeginDisabled(!libraryOkay);
        if (ImGui::Button("Clear this assignment")) {
            auto next = store;
            std::erase_if(next.assignments, [&](const auto& a) {
                return a.weapon == draft.weapon && a.family == draft.family && a.hand == assignmentHand;
            });
            persist(std::move(next),
                    "Assignment cleared. A more general family or either-hand assignment may still apply.");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("Copy share code")) {
            ImGui::SetClipboardText(WeaponShareCode(draft).c_str());
            status = "Share code copied. It contains this preset, without your game files or active assignments.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Export file")) {
            const auto file = ChooseFile(true);
            if (!file.empty()) {
                std::error_code compareError;
                if (std::filesystem::equivalent(file, libraryFile(), compareError)) {
                    error = "Export to a separate preset file, not your library.json.";
                } else {
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
        ImGui::EndDisabled();
        if (dirty)
            ImGui::TextColored(ImVec4(1, .76f, .38f, 1), "Unsaved draft");
        if (!valid)
            ImGui::TextWrapped("%s", why.c_str());
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(.16f, .14f, .10f, 1));
        ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(.42f, .34f, .18f, 1));
        ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(.5f, .41f, .22f, 1));
        ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, ImVec4(.78f, .64f, .34f, 1));
        if (ImGui::BeginTabBar("preset-sections")) {
            if (ImGui::BeginTabItem("Tuning")) {
                tuning(*w);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Actions & triggers")) {
                rules();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Test & live")) {
                test(*w);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::PopStyleColor(4);
    }
    void draw(const std::wstring& dir, const std::wstring& ini, ImVec2 origin, ImVec2 size, float scale) {
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
        ImGui::TextDisabled("%zu real weapons  |  %s", catalog.weapons.size(), live ? "VR connected" : "Local presets");
        if (ImGui::Button("Import preset")) {
            paste[0] = 0;
            importReady = false;
            ImGui::OpenPopup("Import weapon preset");
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload library"))
            reloadLibrary();
        ImGui::SameLine();
        ImGui::TextDisabled("English game names");
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
        if (!isLoading && catalog.weapons.empty() && ImGui::Button("Retry catalog"))
            start(dir, ini);
        const float width = std::clamp(size.x * .32f, 240 * scale, 350 * scale);
        ImGui::BeginChild("catalog", ImVec2(width, 0), ImGuiChildFlags_Borders);
        ImGui::SetNextItemWidth(-1);
        EditText("##search", search);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Search weapon name or numeric ID");
        if (search.empty() && !ImGui::IsItemActive()) {
            auto pos = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + 8 * scale, pos.y + 8 * scale),
                                                ImGui::GetColorU32(ImGuiCol_TextDisabled), "Search weapons or ID...");
        }
        std::set<uint16_t> families;
        for (const auto& w : catalog.weapons)
            families.insert(w.type);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##family",
                              familyFilter ? WeaponTypeName((uint16_t)familyFilter) : "All weapon families")) {
            if (ImGui::Selectable("All weapon families", !familyFilter))
                familyFilter = 0;
            for (auto type : families)
                if (ImGui::Selectable(WeaponTypeName(type), familyFilter == type))
                    familyFilter = type;
            ImGui::EndCombo();
        }
        ImGui::Checkbox("Customized", &customOnly);
        ImGui::SameLine();
        ImGui::Checkbox("Equipped", &equippedOnly);
        const auto query = Lower(search);
        std::vector<const WeaponCatalogEntry*> filtered;
        for (const auto& w : catalog.weapons)
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
                const auto& w = *filtered[i];
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
                ImGui::TextDisabled("%s%s", WeaponTypeName(w.type), customized(w) ? " *" : "");
                ImGui::EndGroup();
                ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + row - ImGui::GetStyle().ItemSpacing.y));
                ImGui::Dummy(ImVec2(0, 0));
                ImGui::PopID();
            }
        if (filtered.empty())
            ImGui::TextWrapped(equippedOnly && !live ? "No live VR equipment data yet." : "No matching weapons.");
        ImGui::EndChild();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("weapon-editor", ImVec2(0, 0), ImGuiChildFlags_Borders);
        editor(scale);
        ImGui::EndChild();
        if (pendingSelection)
            ImGui::OpenPopup("Unsaved weapon draft");
        if (ImGui::BeginPopupModal("Unsaved weapon draft", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Save or discard this draft before changing weapons.");
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
            ImGui::EndPopup();
        }
        importModal(scale);
        ImGui::EndChild();
    }
};
WeaponWorkshop::WeaponWorkshop(ID3D11Device* device) : state_(std::make_unique<State>(device)) {}
WeaponWorkshop::~WeaponWorkshop() = default;
void WeaponWorkshop::draw(const std::wstring& dir, const std::wstring& ini, ImVec2 origin, ImVec2 size, float scale) {
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(.82f, .69f, .40f, 1));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(.28f, .23f, .13f, 1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(.25f, .22f, .15f, 1));
    state_->draw(dir, ini, origin, size, scale);
    ImGui::PopStyleColor(3);
}
} // namespace launcher
