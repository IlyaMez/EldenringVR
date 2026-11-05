#pragma once
#include "system.hpp"
#include "update.hpp"
#include "imgui.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace launcher {

// The launcher's print: an engraved serif for names and headlines, a condensed sans for labels, and Windows'
// own for the rest.
struct Fonts {
    ImFont* body{};      // Barlow Medium: what the launcher says
    ImFont* semibold{};  // Barlow SemiBold: the names of options
    ImFont* label{};     // Barlow Condensed Bold: buttons and row titles
    ImFont* kicker{};    // Barlow Condensed SemiBold: card titles and small lines in capitals
    ImFont* display{};   // Cinzel Bold: the name ERVR, headlines, tabs, START VR
    ImFont* narration{}; // Georgia Italic (Windows'): the narrator's lines
    ImFont* mono{};      // Consolas: the session log
    bool icons{};
};

// Colours and metrics before DPI scaling.
void theme(ImGuiStyle& style);

class App {
public:
    // `updatedFrom`: the version an update replaced (--updated-from), empty on an ordinary start.
    App(HWND window, Fonts fonts, std::string updatedFrom = {});
    ~App();
    // Reads the scan, the checks and the session; runs every loop, even while minimized.
    void poll();
    void frame(float scale);
    // Something on screen moves (a check or a session): draw more often in the background.
    bool busy();
    bool shouldQuit() const { return quit_; }
    // After an update installed itself: the folder whose launcher starts once this one has closed; empty otherwise.
    std::wstring restartFolder() const { return update_.state == UpdateState::installed ? installFolder_ : std::wstring(); }

private:
    enum class Tab { play, options, controls, about };
    enum class Mark { ok, warning, error, busy, info };
    // A button in the gold of the one thing to do, keylined in gold (the tip jar), in red (removing), or plain.
    enum class Ink { plain, primary, support, danger };
    // The caption's bar: the narrator's gold, red for what went wrong, grey for plain facts.
    enum class Caption { narrator, alert, fact };
    // How a sticker is printed: its fill, its fill under the pointer, lettering, keyline and drop (0: none).
    struct Paint {
        ImU32 fill, hover, lettering, keyline, drop;
    };
    // What keeps START VR from starting, and what it does first.
    struct Readiness {
        std::vector<std::string> blockers;
        bool checking{};      // the scan is still running
        bool needsSwitch{};   // flat mods in the way or ERVR parked: the game folder switches to VR first (asks)
        bool needsInstall{};  // ERVR missing or older: installed first
    };
    // A modal question.
    enum class Ask { none, switchToVr, gameRunning, flat, uninstall, defaults, administrator };

    float S(float value) const { return value * scale_; }
    void header();
    void tabs();
    void play(ImVec2 origin, ImVec2 size);
    void options(ImVec2 origin, ImVec2 size);
    void controls(ImVec2 origin, ImVec2 size);
    void about(ImVec2 origin, ImVec2 size);
    // START VR, what the session does in words, and how far it got.
    void startPanel(ImVec2 size);
    void setupCard(float width);
    void logCard(ImVec2 size);
    void shortcutBanner(float width);
    // A newer ERVR on GitHub, its download and install, or the update just done; returns its height (0: none).
    float updateBanner(float width);
    // The About tab's Updates card: the last check, the start-up check switch, Check now / Update now.
    void updatesCard(float width);
    // Why Update now cannot run right now; empty when it can.
    std::string updateBlocker();
    void supportCards(float width);
    void modals();

    bool beginCard(const char* id, ImVec2 size, int childFlags = 0);
    void endCard();
    void cardTitle(const char* title, float rightWidth = 0);
    void setupRow(Mark mark, const char* title, const std::string& detail, float actionWidth,
                  const std::function<void()>& action, bool last = false);
    // `warning`: the help is a warning (amber).
    void option(const char* title, const char* help, float controlWidth, const std::function<void()>& control,
                bool warning = false);
    void statusMark(ImDrawList* draw, ImVec2 center, Mark mark);
    // A button in label capitals; "label##id" as ImGui's own; `icon` is a Segoe glyph or null.
    bool button(const char* label, float width, Ink ink = Ink::plain, const char* icon = nullptr);
    float buttonWidth(const char* label, const char* icon = nullptr);
    // A big button in display capitals, with an optional small line under them; with a drop it sinks onto it
    // while pressed.
    bool sticker(const char* id, const char* icon, const char* label, const char* line, ImVec2 size,
                 const Paint& paint, bool enabled = true);
    bool toggle(const char* id, bool* value);
    // ImGui's slider, drawn as a gauge filled up to the value.
    bool slider(const char* id, float* value, float lowest, float highest, const char* format);
    bool combo(const char* id, int* index, const std::vector<std::string>& labels);
    bool link(const char* label);
    // The narrator's words at `at`, `width` wide, beside a bar of the caption's colour, `emphasis` after them in
    // gold (red for an alert); returns their height, and with no draw list only measures them.
    float captionBox(ImDrawList* draw, ImVec2 at, float width, const std::string& text,
                     Caption paper = Caption::narrator, const std::string& emphasis = {});
    // A pill stating one fact, its left edge at `at.x`, centred on `at.y`; a `dot` colour pulses before the words.
    // Returns its width.
    float stamp(ImDrawList* draw, ImVec2 at, const char* text, ImU32 fill, ImU32 lettering, ImU32 keyline, float size,
                ImU32 dot = 0);
    // The name ERVR, `size` high; returns its width.
    float wordmark(ImDrawList* draw, ImVec2 at, float size);
    void emblem(ImDrawList* draw, ImVec2 center, float radius);

    // ERVR.ini, as the Options tab reads and edits it.
    void loadIni();
    std::string iniValue(const char* section, const char* key, const char* fallback) const;
    void setIni(const char* section, const char* key, const std::string& value);
    void saveIni();

    Readiness readiness();
    // The runtime this start uses: the one chosen, or Automatic's pick (null: Windows' active one).
    const Runtime* sessionRuntime();
    const Runtime* chosenRuntime();
    std::string runtimeWords();
    // What "Copy report" copies: ERVR's version, the game, this PC and the last session, in a few lines.
    std::string report();
    // START VR: asks first when the game runs or the folder needs switching, then begins.
    void start();
    void begin(bool switchFirst, bool attach);
    // A change to the game folder from a button: its lines go to the log, then the folder is read again.
    void apply(const Change& change, const char* what);
    // The chosen game effects turned off now (the game must be closed), what changed noted in the log.
    void turnOffEffects();
    void rescan();
    void save() { saveSettings(settings_); }
    void copyLog();
    void note(LineKind kind, std::string text);

    HWND window_;
    Fonts fonts_;
    float scale_ = 1;
    Tab tab_ = Tab::play;
    Settings settings_;
    Scanner scanner_;
    Scan scan_;
    bool scanning_ = true;
    HeadsetCheck headset_;
    Session session_;
    std::vector<LogLine> log_;
    size_t logCount_{};
    bool scrollLog_{};
    bool sessionRan_{};
    std::chrono::steady_clock::time_point gameSeenAt_{};
    std::string startError_;
    bool quit_{};
    bool gameRunning_{};
    uint64_t lastGameCheck_{};
    Ask ask_ = Ask::none;
    std::string administratorError_;
    std::string shortcutMessage_;
    // ImGui's time when "Copy report" last copied: its button says "Copied" for two seconds.
    double reportCopied_ = -2;
    // ERVR.ini: the file, an edit not yet written and since when, and why the last write failed.
    std::optional<IniFile> ini_;
    bool iniDirty_{};
    double iniEditedAt_{};
    std::string iniError_;
    uint64_t lastIniCheck_{};
    // The option rows of a card draw a rule between them, not above the first.
    bool firstOption_ = true;
    // Updates: the folder an update replaces (empty: this launcher cannot update itself, and why), the updater and
    // its last status, "Later" for this run, and the version an update just replaced.
    std::string installWhyNot_;
    std::wstring installFolder_;
    Updater updater_;
    UpdateStatus update_;
    bool updateLater_{};
    std::string updatedFrom_;
};

} // namespace launcher
