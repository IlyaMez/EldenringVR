#include "ui.hpp"
#include "imgui_internal.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>

namespace launcher {
namespace {

// Segoe MDL2 Assets / Segoe Fluent Icons glyphs.
constexpr const char* kHeart = "\xEE\xAD\x92";
constexpr const char* kPlay = "\xEE\x9D\xA8";
constexpr const char* kStop = "\xEE\x9C\x9A";
constexpr const char* kFolder = "\xEE\xA2\xB7";
constexpr const char* kRefresh = "\xEE\x9C\xAC";
constexpr const char* kCopy = "\xEE\xA3\x88";
constexpr const char* kDocument = "\xEE\xA2\xA5";
constexpr const char* kShortcut = "\xEE\x9C\x9B";
constexpr const char* kDelete = "\xEE\x9D\x8D";
constexpr const char* kSwitch = "\xEE\xA2\xAB";

// The header and the tab bar, before DPI scaling.
constexpr float kHeaderHeight = 94, kTabsHeight = 50;

struct Rgb {
    int r, g, b;
};
// The Lands Between at night: a near-black ground with a warm cast, ash-brown surfaces, parchment for words, and
// gold for the one thing to do, for what is switched on and for the narrator. Green says fine, amber needs a
// look, red is a fault.
constexpr Rgb kGround{12, 11, 9}, kSurface{21, 19, 16}, kSurfaceHigh{32, 29, 24}, kSurfaceHover{45, 40, 32},
    kLine{60, 53, 40}, kWell{14, 13, 11}, kInk{24, 17, 7}, kPaper{238, 230, 210}, kPaperDim{178, 168, 146},
    kFaint{130, 121, 103}, kLogText{206, 199, 181}, kGold{201, 164, 90}, kGoldLift{230, 196, 122},
    kGoldDeep{150, 117, 58}, kGoldDrop{84, 62, 27}, kGreen{143, 196, 134}, kAmber{236, 178, 76}, kRed{186, 58, 46},
    kRedLift{228, 96, 80}, kWhite{255, 255, 255};

// A colour faded with the current style alpha, so disabled custom widgets fade too.
ImU32 col(Rgb c, float alpha = 1) {
    const float a = std::clamp(alpha * ImGui::GetStyle().Alpha, 0.f, 1.f);
    return IM_COL32(c.r, c.g, c.b, static_cast<int>(a * 255));
}

ImVec4 vec(Rgb c, float alpha = 1) { return ImVec4(c.r / 255.f, c.g / 255.f, c.b / 255.f, alpha); }

ImVec2 add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }

std::string format(const char* pattern, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, pattern);
    vsnprintf(buffer, sizeof(buffer), pattern, args);
    va_end(args);
    return buffer;
}

ImVec2 measure(ImFont* font, float size, const char* text, float wrap = 0) {
    return font->CalcTextSizeA(size, FLT_MAX, wrap, text);
}

void handCursor() {
    if (ImGui::IsItemHovered())
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
}

// Capitals (ASCII letters; the rest stays).
std::string upper(std::string_view text) {
    std::string out(text);
    for (char& c : out)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return out;
}

// Text with `spacing` em between its letters, as labels (0.05) and kickers (0.14) are set.
float spacedWidth(ImFont* font, float size, const char* text, float spacing) {
    float width = 0;
    for (const char* s = text; *s;) {
        unsigned int c = 0;
        const int n = ImTextCharFromUtf8(&c, s, nullptr);
        width += font->CalcTextSizeA(size, FLT_MAX, 0, s, s + n).x + (s[n] ? spacing * size : 0);
        s += n;
    }
    return width;
}

void spacedText(ImDrawList* draw, ImFont* font, float size, ImVec2 at, ImU32 color, const char* text, float spacing) {
    for (const char* s = text; *s;) {
        unsigned int c = 0;
        const int n = ImTextCharFromUtf8(&c, s, nullptr);
        draw->AddText(font, size, at, color, s, s + n);
        at.x += font->CalcTextSizeA(size, FLT_MAX, 0, s, s + n).x + spacing * size;
        s += n;
    }
}

// Lettering cut into the surface: a dark shadow under it, a little down and right.
void engraved(ImDrawList* draw, ImFont* font, float size, ImVec2 at, ImU32 color, const char* text, float spacing = 0) {
    const float offset = std::max(1.f, std::round(size * 0.035f));
    const ImVec2 shadow(at.x + offset, at.y + offset);
    if (spacing > 0) {
        spacedText(draw, font, size, shadow, col(kInk, 0.95f), text, spacing);
        spacedText(draw, font, size, at, color, text, spacing);
    } else {
        draw->AddText(font, size, shadow, col(kInk, 0.95f), text);
        draw->AddText(font, size, at, color, text);
    }
}

// Grace's light: `tint` at `strength` in the middle, fading to nothing at `radius`.
void glow(ImDrawList* draw, ImVec2 c, float radius, Rgb tint, float strength) {
    constexpr int segments = 48;
    const ImU32 inner = col(tint, strength), middle = col(tint, strength * 0.42f), outer = col(tint, 0);
    const ImVec2 uv = draw->_Data->TexUvWhitePixel;
    draw->PrimReserve(segments * 9, segments * 2 + 1);
    const auto base = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
    draw->PrimWriteVtx(c, uv, inner);
    for (int ring = 0; ring < 2; ++ring)
        for (int i = 0; i < segments; ++i) {
            const float a = i * 6.2831853f / segments, r = ring ? radius : radius * 0.38f;
            draw->PrimWriteVtx(ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r), uv, ring ? outer : middle);
        }
    for (int i = 0; i < segments; ++i) {
        const auto a = static_cast<ImDrawIdx>(base + 1 + i), b = static_cast<ImDrawIdx>(base + 1 + (i + 1) % segments);
        const auto c2 = static_cast<ImDrawIdx>(a + segments), d = static_cast<ImDrawIdx>(b + segments);
        draw->PrimWriteIdx(base);
        draw->PrimWriteIdx(a);
        draw->PrimWriteIdx(b);
        draw->PrimWriteIdx(a);
        draw->PrimWriteIdx(c2);
        draw->PrimWriteIdx(d);
        draw->PrimWriteIdx(a);
        draw->PrimWriteIdx(d);
        draw->PrimWriteIdx(b);
    }
}

// A level rule that fades out toward both ends.
void rule(ImDrawList* draw, ImVec2 a, float width, Rgb tint, float alpha, float thickness = 1) {
    const ImU32 clear = col(tint, 0), solid = col(tint, alpha);
    const float half = width * 0.5f;
    draw->AddRectFilledMultiColor(a, ImVec2(a.x + half, a.y + thickness), clear, solid, solid, clear);
    draw->AddRectFilledMultiColor(ImVec2(a.x + half, a.y), ImVec2(a.x + width, a.y + thickness), solid, clear, clear,
                                  solid);
}

void diamond(ImDrawList* draw, ImVec2 c, float r, ImU32 color, bool filled = true, float thickness = 1) {
    const ImVec2 top(c.x, c.y - r), right(c.x + r, c.y), bottom(c.x, c.y + r), left(c.x - r, c.y);
    if (filled)
        draw->AddQuadFilled(top, right, bottom, left, color);
    else
        draw->AddQuad(top, right, bottom, left, color, thickness);
}

// Short gold brackets in a box's corners, as the game frames its own menus.
void corners(ImDrawList* draw, ImVec2 min, ImVec2 max, float length, ImU32 color, float thickness) {
    const float t = thickness * 0.5f;
    draw->AddLine(ImVec2(min.x, min.y + t), ImVec2(min.x + length, min.y + t), color, thickness);
    draw->AddLine(ImVec2(min.x + t, min.y), ImVec2(min.x + t, min.y + length), color, thickness);
    draw->AddLine(ImVec2(max.x, min.y + t), ImVec2(max.x - length, min.y + t), color, thickness);
    draw->AddLine(ImVec2(max.x - t, min.y), ImVec2(max.x - t, min.y + length), color, thickness);
    draw->AddLine(ImVec2(min.x, max.y - t), ImVec2(min.x + length, max.y - t), color, thickness);
    draw->AddLine(ImVec2(min.x + t, max.y), ImVec2(min.x + t, max.y - length), color, thickness);
    draw->AddLine(ImVec2(max.x, max.y - t), ImVec2(max.x - length, max.y - t), color, thickness);
    draw->AddLine(ImVec2(max.x - t, max.y), ImVec2(max.x - t, max.y - length), color, thickness);
}

// A tooltip, wrapped.
void tip(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_PopupBg, vec(kSurfaceHover));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(kGoldDeep));
    ImGui::PushStyleColor(ImGuiCol_Text, vec(kPaper));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y));
    if (ImGui::BeginTooltip()) {
        ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0], 14);
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

// A number as ERVR.ini spells it: up to `decimals` places, at least one ("1.0", "0.86").
std::string number(float value, int decimals) {
    std::string text = format("%.*f", decimals, value);
    while (text.size() > 1 && text.back() == '0' && text[text.size() - 2] != '.')
        text.pop_back();
    return text;
}

bool truthy(const std::string& value) {
    return text::sameText(value, "1") || text::sameText(value, "true") || text::sameText(value, "yes") ||
           text::sameText(value, "on");
}

std::string join(const std::vector<std::string>& items, const char* separator) {
    std::string out;
    for (const auto& item : items)
        out += (out.empty() ? "" : separator) + item;
    return out;
}

// One of the game's quality words as people say it: "max", "high", "off".
std::string effectWord(const std::string& value) {
    if (text::sameText(value, text::kEffectOff))
        return "off";
    std::string out = value;
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

// What each of text::kVrEffects is called in Options, and why VR is better without it.
constexpr std::array<std::pair<const char*, const char*>, text::kVrEffects.size()> kEffectRows{{
    {"Turn off depth of field", "The game blurs the distance and what it guesses you are not looking at; in a "
                                "headset your eyes do the focusing."},
    {"Turn off motion blur", "It smears the picture whenever the view moves, and in VR your head moves it all the time."},
    {"Turn off ray tracing", "The costliest setting the game has, and ERVR draws every frame twice."},
}};

} // namespace

void theme(ImGuiStyle& style) {
    style.WindowPadding = ImVec2(16, 16);
    style.FramePadding = ImVec2(10, 7);
    style.ItemSpacing = ImVec2(10, 10);
    style.ItemInnerSpacing = ImVec2(8, 6);
    // Boxes are square, as the game's are; only buttons and pills round their corners a little.
    style.WindowRounding = 0;
    style.ChildRounding = 0;
    style.FrameRounding = 0;
    style.PopupRounding = 0;
    style.GrabRounding = 0;
    style.ScrollbarRounding = 0;
    style.ScrollbarSize = 10;
    style.GrabMinSize = 12;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 1;
    style.DisabledAlpha = 0.45f;
    style.FontSizeBase = 15;
    style.SeparatorTextBorderSize = 1;
    auto* c = style.Colors;
    c[ImGuiCol_Text] = vec(kPaper);
    c[ImGuiCol_TextDisabled] = vec(kFaint);
    c[ImGuiCol_WindowBg] = vec(kGround);
    c[ImGuiCol_ChildBg] = vec(kSurface);
    c[ImGuiCol_PopupBg] = vec(kSurfaceHigh);
    c[ImGuiCol_Border] = vec(kLine);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = vec(kSurfaceHigh);
    c[ImGuiCol_FrameBgHovered] = vec(kSurfaceHover);
    c[ImGuiCol_FrameBgActive] = vec(kSurfaceHover);
    c[ImGuiCol_Button] = vec(kSurfaceHigh);
    c[ImGuiCol_ButtonHovered] = vec(kSurfaceHover);
    c[ImGuiCol_ButtonActive] = vec(kGoldDeep);
    c[ImGuiCol_Header] = vec(kGoldDeep, 0.55f);
    c[ImGuiCol_HeaderHovered] = vec(kSurfaceHover);
    c[ImGuiCol_HeaderActive] = vec(kGoldDeep);
    c[ImGuiCol_SliderGrab] = vec(kGold);
    c[ImGuiCol_SliderGrabActive] = vec(kGoldLift);
    c[ImGuiCol_CheckMark] = vec(kGold);
    c[ImGuiCol_ScrollbarBg] = vec(kGround, 0);
    c[ImGuiCol_ScrollbarGrab] = vec(kSurfaceHover);
    c[ImGuiCol_ScrollbarGrabHovered] = vec(kLine);
    c[ImGuiCol_ScrollbarGrabActive] = vec(kGoldDeep);
    c[ImGuiCol_Separator] = vec(kLine);
    c[ImGuiCol_TextSelectedBg] = vec(kGoldDeep, 0.85f);
    c[ImGuiCol_ModalWindowDimBg] = vec(kGround, 0.82f);
    c[ImGuiCol_NavCursor] = vec(kGoldLift);
}

App::App(HWND window, Fonts fonts) : window_(window), fonts_(fonts), settings_(loadSettings()) {
    scanner_.start(settings_);
}

App::~App() = default;

void App::rescan() {
    headset_.reset();
    scanning_ = true;
    scanner_.start(settings_);
}

void App::poll() {
    const bool scanning = scanner_.busy();
    if (scanning_ && !scanning) {
        scan_ = scanner_.snapshot();
        // A runtime chosen before that is no longer installed gives way to Automatic; Seamless Co-op removed, to
        // the offline start.
        if (settings_.runtime != kAutoRuntime && !chosenRuntime())
            settings_.runtime = kAutoRuntime;
        if (settings_.mode == StartMode::coop && !scan_.coop)
            settings_.mode = StartMode::offline;
        gameRunning_ = scan_.gameRunning;
        if (!iniDirty_)
            loadIni();
    }
    scanning_ = scanning;
    const ULONGLONG now = GetTickCount64();
    // The game started or closed elsewhere (Steam's Play button, a crash).
    if (scan_.done && !scan_.gameDir.empty() && now - lastGameCheck_ > 2000) {
        lastGameCheck_ = now;
        gameRunning_ = gameRunning(scan_.gameDir);
    }
    // ERVR.ini changed elsewhere (an editor, ERVR's own SaveLearned): read it again.
    if (ini_ && !iniDirty_ && now - lastIniCheck_ > 1000) {
        lastIniCheck_ = now;
        if (writeTime(ini_->path) != ini_->stamp)
            loadIni();
    }
    std::vector<LogLine> fresh;
    const size_t total = session_.lines(logCount_, fresh);
    if (total < logCount_)
        log_.clear(); // the session dropped its oldest lines and sent the rest again
    if (!fresh.empty()) {
        log_.insert(log_.end(), fresh.begin(), fresh.end());
        scrollLog_ = true;
    }
    logCount_ = total;
    const bool running = session_.running();
    if (running && session_.phase() == Session::Phase::game && gameSeenAt_ == std::chrono::steady_clock::time_point{})
        gameSeenAt_ = std::chrono::steady_clock::now();
    // The session ended: the game folder may have changed (ERVR writes its cache and backups).
    if (sessionRan_ && !running)
        rescan();
    sessionRan_ = running;
}

bool App::busy() { return session_.running() || scanner_.busy() || headset_.state() == Outcome::running; }

const Runtime* App::chosenRuntime() {
    for (const auto& r : scan_.runtimes)
        if (_wcsicmp(r.manifest.c_str(), settings_.runtime.c_str()) == 0)
            return &r;
    return nullptr;
}

const Runtime* App::sessionRuntime() {
    if (settings_.runtime != kAutoRuntime)
        if (const Runtime* r = chosenRuntime())
            return r;
    return automaticRuntime(scan_);
}

std::string App::runtimeWords() {
    std::string active;
    for (const auto& r : scan_.runtimes)
        if (r.active)
            active = r.name;
    const Runtime* now = sessionRuntime();
    return now ? now->name : active.empty() ? "Windows' active runtime" : active + " (Windows' active)";
}

App::Readiness App::readiness() {
    Readiness r;
    if (!scan_.done) {
        r.checking = true;
        r.blockers.push_back("Checking this PC...");
        return r;
    }
    const Install& in = scan_.install;
    const bool payload = !scan_.payload.empty() && scan_.payloadMissing.empty();
    if (scan_.gameExe.empty())
        r.blockers.push_back("ELDEN RING was not found: show the launcher where Game\\eldenring.exe is.");
    else {
        r.needsSwitch = in.state == InstallState::parked ||
                        std::any_of(in.flat.begin(), in.flat.end(), [](const ModFile& m) { return !m.parked; });
        r.needsInstall = !scan_.payloadInGame && payload &&
                         (in.state == InstallState::missing || in.state == InstallState::older || !in.reshade || !in.ini);
        if (in.state == InstallState::missing && !payload)
            r.blockers.push_back("ERVR is not installed, and this folder lacks its files: extract the whole zip again.");
        if ((r.needsInstall || r.needsSwitch) && !scan_.writable)
            r.blockers.push_back("The launcher can't change files in the game folder: restart it as administrator.");
    }
    if (scan_.steam.empty())
        r.blockers.push_back("Steam was not found: ELDEN RING needs it running.");
    if (scan_.runtimes.empty())
        r.blockers.push_back("No VR runtime is installed: Meta Quest Link, Virtual Desktop or SteamVR.");
    return r;
}

std::string App::report() {
    text::ReportFacts facts;
    facts.version = kVersion;
    if (scan_.done) {
        facts.game = scan_.gameExe.empty()
                         ? "not found"
                         : scan_.gameSource + ", exe " + scan_.gameVersion +
                               (scan_.gamePatch.empty() ? " (no verified address map)"
                                                        : " (patch " + scan_.gamePatch + ", verified)");
        const Install& in = scan_.install;
        static constexpr const char* states[] = {"not installed", "current", "older", "newer", "another build",
                                                 "parked for flat play"};
        facts.install = (in.version.empty() ? std::string() : in.version + ", ") + states[static_cast<int>(in.state)] +
                        "; launcher carries " + (scan_.payloadVersion.empty() ? "none" : scan_.payloadVersion) +
                        "; dxgi.dll " + (in.reshade ? "bundled ReShade" : in.dxgi.empty() ? "missing" : in.dxgi) +
                        (in.foreignProxy ? "; another dinput8.dll loader" : "");
        facts.runtime = (settings_.runtime == kAutoRuntime ? "Automatic, now " : "") + runtimeWords();
        for (const auto& r : scan_.runtimes)
            facts.runtime += (&r == &scan_.runtimes.front() ? "; installed: " : ", ") + r.name;
        std::vector<std::string> live;
        for (const auto& mod : in.flat)
            if (!mod.parked)
                live.push_back(mod.label);
        facts.others = live.empty() ? "none active" : join(live, ", ");
    }
    facts.mode = settings_.mode == StartMode::coop ? "Seamless Co-op" : "offline, EAC off";
    const Progress progress = session_.progress();
    if (headset_.state() == Outcome::ok) {
        const auto r = headset_.result();
        facts.headset = r.headset + " via " + r.runtime + ", " + r.eye + " per eye";
    } else if (!progress.headset.empty()) {
        facts.headset = progress.headset + " via " + progress.runtime + ", " + progress.eye + " per eye (last session)";
    } else {
        facts.headset = "not checked";
    }
    const Hardware pc = hardware();
    facts.gpu = pc.gpu.empty() ? "" : pc.gpu + format(", %.1f GB", pc.gpuMemoryMb / 1024.0);
    facts.ram = pc.ramMb ? format("%.1f GB", pc.ramMb / 1024.0) : "";
    std::vector<LogLine> lines = log_;
    if (lines.empty())
        lines = lastLog(scan_.gameDir);
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (facts.lastLine.empty() && !it->text.empty())
            facts.lastLine = it->text;
        if (it->kind == LineKind::error) {
            facts.lastError = it->text;
            break;
        }
    }
    return text::reportSummary(facts);
}

void App::note(LineKind kind, std::string text) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    log_.push_back({format("%02u:%02u:%02u", static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute),
                           static_cast<unsigned>(now.wSecond)),
                    kind, std::move(text)});
    scrollLog_ = true;
}

void App::apply(const Change& change, const char* what) {
    note(change.ok ? LineKind::good : LineKind::error, std::string(what) + (change.ok ? ":" : " stopped:"));
    for (const auto& line : change.lines)
        note(change.ok ? LineKind::normal : LineKind::error, "  " + line);
    if (change.accessDenied)
        ask_ = Ask::administrator;
    refreshInstall(scan_);
    loadIni();
    rescan();
}

void App::turnOffEffects() {
    const Change change = turnOffGameEffects(scan_, settings_.effectsOff);
    for (const auto& line : change.lines)
        note(change.ok ? LineKind::normal : LineKind::error, line);
    scan_.graphics = readGameGraphics();
}

void App::start() {
    startError_.clear();
    administratorError_.clear();
    if (gameRunning_) {
        ask_ = Ask::gameRunning;
        return;
    }
    if (readiness().needsSwitch) {
        ask_ = Ask::switchToVr;
        return;
    }
    begin(false, false);
}

void App::begin(bool switchFirst, bool attach) {
    LaunchPlan plan;
    plan.gameDir = scan_.gameDir;
    plan.gameExe = scan_.gameExe;
    plan.steamExe = scan_.steam;
    plan.mode = settings_.mode;
    plan.attach = attach;
    if (const Runtime* r = sessionRuntime()) {
        plan.runtimeManifest = r->manifest;
        plan.runtimeName = r->name;
    }
    const auto fail = [&](const Change& change) {
        startError_ = join(change.lines, " ");
        if (change.accessDenied)
            ask_ = Ask::administrator;
        refreshInstall(scan_);
    };
    if (!attach) {
        if (switchFirst) {
            const Change change = switchToVr(scan_);
            if (!change.ok)
                return fail(change);
            plan.before.push_back("Switched the game folder to VR:");
            for (const auto& line : change.lines)
                plan.before.push_back("  " + line);
            refreshInstall(scan_);
        }
        if (readiness().needsInstall) {
            const Change change = installErvr(scan_);
            if (!change.ok)
                return fail(change);
            plan.before.push_back("Installed into the game folder:");
            for (const auto& line : change.lines)
                plan.before.push_back("  " + line);
            refreshInstall(scan_);
            loadIni();
        }
        // The game's effects that work against VR, off before the game reads its settings.
        const Change graphics = turnOffGameEffects(scan_, settings_.effectsOff);
        plan.before.insert(plan.before.end(), graphics.lines.begin(), graphics.lines.end());
        scan_.graphics = readGameGraphics();
    }
    log_.clear();
    logCount_ = 0;
    gameSeenAt_ = {};
    if (session_.start(std::move(plan), startError_)) {
        save();
        sessionRan_ = true;
    }
}

void App::copyLog() {
    std::string all;
    for (const auto& line : log_)
        all += (line.time.empty() ? "" : line.time + "  ") + line.text + "\n";
    ImGui::SetClipboardText(all.empty() ? startError_.c_str() : all.c_str());
}

// --- ERVR.ini ------------------------------------------------------------------------------------------------

void App::loadIni() {
    ini_ = readIni(iniPath(scan_));
    iniDirty_ = false;
}

std::string App::iniValue(const char* section, const char* key, const char* fallback) const {
    if (!ini_)
        return fallback;
    const auto value = text::iniGet(ini_->text, section, key);
    return value ? *value : fallback;
}

void App::setIni(const char* section, const char* key, const std::string& value) {
    if (!ini_ || text::iniGet(ini_->text, section, key) == value)
        return;
    ini_->text = text::iniSet(ini_->text, section, key, value);
    iniDirty_ = true;
    iniEditedAt_ = ImGui::GetTime();
}

void App::saveIni() {
    if (!ini_ || !iniDirty_)
        return;
    iniDirty_ = false;
    iniError_ = writeIni(*ini_) ? std::string()
                                : "ERVR.ini could not be saved (Windows refused the file): the change is not in it.";
}

// --- Widgets -----------------------------------------------------------------------------------------------------

bool App::beginCard(const char* id, ImVec2 size, int childFlags) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(kSurface));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(kLine));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(16)));
    const bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | childFlags, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    if (open) {
        // A faint light in its top right corner, and gold brackets at its corners.
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 min = ImGui::GetWindowPos(), max = add(min, ImGui::GetWindowSize());
        draw->PushClipRect(min, max);
        glow(draw, ImVec2(max.x, min.y), S(220), kGold, 0.06f);
        corners(draw, min, max, S(9), col(kGoldDeep, 0.9f), S(1.5f));
        draw->PopClipRect();
    }
    firstOption_ = true;
    return open;
}

void App::endCard() { ImGui::EndChild(); }

// A card's name: small spaced capitals after a gold diamond.
void App::cardTitle(const char* title, float rightWidth) {
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFrameHeight(), size = S(14);
    const float y = at.y + (height - size) * 0.5f;
    diamond(draw, ImVec2(at.x + S(4), y + size * 0.52f), S(4), col(kGold));
    spacedText(draw, fonts_.kicker, size, ImVec2(at.x + S(16), y), col(kPaperDim), upper(title).c_str(), 0.14f);
    // The caller may put buttons on the right of the title line.
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x - rightWidth, height));
    if (rightWidth > 0)
        ImGui::SameLine(0, 0);
}

// A dark disc with the mark in its colour; only a fault fills it, to be seen first.
void App::statusMark(ImDrawList* draw, ImVec2 c, Mark mark) {
    const float r = S(11), w = S(2.2f);
    switch (mark) {
    case Mark::ok:
        draw->AddCircleFilled(c, r, col(kSurfaceHover), 24);
        draw->PathLineTo(ImVec2(c.x - r * 0.42f, c.y + r * 0.02f));
        draw->PathLineTo(ImVec2(c.x - r * 0.1f, c.y + r * 0.34f));
        draw->PathLineTo(ImVec2(c.x + r * 0.46f, c.y - r * 0.32f));
        draw->PathStroke(col(kGreen), 0, w);
        break;
    case Mark::error:
        draw->AddCircleFilled(c, r, col(kRed), 24);
        draw->AddLine(ImVec2(c.x - r * 0.36f, c.y - r * 0.36f), ImVec2(c.x + r * 0.36f, c.y + r * 0.36f), col(kWhite), w);
        draw->AddLine(ImVec2(c.x + r * 0.36f, c.y - r * 0.36f), ImVec2(c.x - r * 0.36f, c.y + r * 0.36f), col(kWhite), w);
        break;
    case Mark::warning:
        draw->AddCircleFilled(c, r, col(kSurfaceHover), 24);
        draw->AddLine(ImVec2(c.x, c.y - r * 0.5f), ImVec2(c.x, c.y + r * 0.12f), col(kAmber), w);
        draw->AddCircleFilled(ImVec2(c.x, c.y + r * 0.44f), S(1.5f), col(kAmber));
        break;
    case Mark::info:
        draw->AddCircleFilled(c, r, col(kSurfaceHover), 24);
        draw->AddCircleFilled(ImVec2(c.x, c.y - r * 0.42f), S(1.5f), col(kPaper));
        draw->AddLine(ImVec2(c.x, c.y - r * 0.08f), ImVec2(c.x, c.y + r * 0.48f), col(kPaper), w);
        break;
    case Mark::busy: {
        const float a = static_cast<float>(ImGui::GetTime()) * 5.f;
        draw->AddCircle(c, r * 0.72f, col(kLine), 24, w);
        draw->PathArcTo(c, r * 0.72f, a, a + 2.1f, 16);
        draw->PathStroke(col(kGold), 0, w);
        break;
    }
    }
}

float App::buttonWidth(const char* label, const char* icon) {
    const char* end = std::strstr(label, "##");
    const std::string text = upper(end ? std::string_view(label, end - label) : std::string_view(label));
    const float iconWidth = icon && fonts_.icons ? measure(fonts_.label, S(15), icon).x + S(8) : 0;
    return spacedWidth(fonts_.label, S(15), text.c_str(), 0.05f) + iconWidth + S(30);
}

bool App::button(const char* label, float width, Ink ink, const char* icon) {
    const ImVec2 size(width, ImGui::GetFrameHeight());
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, size);
    handCursor();
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    const bool primary = ink == Ink::primary;
    auto* draw = ImGui::GetWindowDrawList();
    // It dips while pressed.
    const ImVec2 sink = held ? ImVec2(0, S(1)) : ImVec2(0, 0);
    const ImVec2 min = add(at, sink), max = add(add(at, size), sink);
    const float rounding = S(3);
    draw->AddRectFilled(min, max, primary ? col(hovered ? kGoldLift : kGold) : col(hovered ? kSurfaceHover : kSurfaceHigh),
                        rounding);
    if (!primary) {
        const ImU32 keyline = ink == Ink::support  ? col(kGold)
                              : ink == Ink::danger ? col(hovered ? kRedLift : kRed)
                                                   : col(hovered ? kGoldDeep : kLine);
        draw->AddRect(min, max, keyline, rounding, 0, S(ink == Ink::plain ? 1 : 1.5f));
    }
    const char* end = std::strstr(label, "##");
    const std::string text = upper(end ? std::string_view(label, end - label) : std::string_view(label));
    const float fontSize = S(15), spacing = 0.05f;
    const float textWidth = spacedWidth(fonts_.label, fontSize, text.c_str(), spacing);
    const float iconWidth = icon && fonts_.icons ? measure(fonts_.label, fontSize, icon).x + S(8) : 0;
    float x = min.x + std::max(S(8), (size.x - textWidth - iconWidth) * 0.5f);
    const float y = min.y + (size.y - fontSize) * 0.5f;
    const ImU32 color = col(primary            ? kInk
                            : ink == Ink::support ? kGoldLift
                            : ink == Ink::danger  ? kRedLift
                                                  : kPaper);
    if (iconWidth > 0) {
        draw->AddText(fonts_.label, fontSize, ImVec2(x, y - S(1)), color, icon);
        x += iconWidth;
    }
    spacedText(draw, fonts_.label, fontSize, ImVec2(x, y), color, text.c_str(), spacing);
    return clicked;
}

bool App::sticker(const char* id, const char* icon, const char* label, const char* line, ImVec2 size,
                  const Paint& paint, bool enabled) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float drop = paint.drop ? std::round(std::max(S(4), size.y * 0.07f)) : 0;
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size.x + drop, size.y + drop));
    ImGui::EndDisabled();
    if (enabled)
        handCursor();
    const bool hovered = enabled && ImGui::IsItemHovered(), held = enabled && ImGui::IsItemActive();
    auto* draw = ImGui::GetWindowDrawList();
    // Pressed, it sinks onto its drop, or dips.
    const float shift = held ? std::max(drop - S(1), S(1)) : 0;
    const ImVec2 min(at.x + shift, at.y + shift), max(at.x + size.x + shift, at.y + size.y + shift);
    const float rounding = S(4);
    if (drop > 0)
        draw->AddRectFilled(ImVec2(at.x + drop, at.y + drop), ImVec2(at.x + size.x + drop, at.y + size.y + drop),
                            enabled ? paint.drop : col(kLine), rounding);
    draw->AddRectFilled(min, max, !enabled ? col(kSurfaceHigh) : hovered ? paint.hover : paint.fill, rounding);
    draw->AddRect(min, max, enabled ? paint.keyline : col(kLine), rounding, 0, S(1.5f));
    const ImU32 lettering = enabled ? paint.lettering : col(kFaint);
    const std::string words = upper(label);
    const float labelSize = line ? size.y * 0.4f : size.y * 0.42f, lineSize = size.y * 0.22f;
    const float iconSize = labelSize * 0.66f;
    const float iconWidth = icon && fonts_.icons ? measure(fonts_.label, iconSize, icon).x + labelSize * 0.36f : 0;
    const float wordsWidth = spacedWidth(fonts_.display, labelSize, words.c_str(), 0.06f);
    const float blockHeight = labelSize * 0.82f + (line ? lineSize * 1.05f : 0);
    const float y = min.y + (size.y - blockHeight) * 0.5f - labelSize * 0.12f;
    float x = min.x + (size.x - wordsWidth - iconWidth) * 0.5f;
    if (iconWidth > 0) {
        draw->AddText(fonts_.label, iconSize, ImVec2(x, y + (labelSize - iconSize) * 0.55f), lettering, icon);
        x += iconWidth;
    }
    spacedText(draw, fonts_.display, labelSize, ImVec2(x, y), lettering, words.c_str(), 0.06f);
    if (line) {
        const std::string small = upper(line);
        const float lineWidth = spacedWidth(fonts_.kicker, lineSize, small.c_str(), 0.12f);
        spacedText(draw, fonts_.kicker, lineSize, ImVec2(min.x + (size.x - lineWidth) * 0.5f, y + labelSize * 0.98f),
                   lettering, small.c_str(), 0.12f);
    }
    return clicked;
}

bool App::toggle(const char* id, bool* value) {
    const float h = S(22), w = S(42);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    if (clicked)
        *value = !*value;
    handCursor();
    const bool hovered = ImGui::IsItemHovered();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + w, at.y + h);
    draw->AddRectFilled(at, end, *value ? col(hovered ? kGoldLift : kGold) : col(hovered ? kSurfaceHover : kSurfaceHigh),
                        h * 0.5f);
    if (!*value)
        draw->AddRect(at, end, col(hovered ? kFaint : kLine), h * 0.5f, 0, S(1));
    const float knob = *value ? end.x - h * 0.5f : at.x + h * 0.5f;
    draw->AddCircleFilled(ImVec2(knob, at.y + h * 0.5f), h * 0.5f - S(4), *value ? col(kInk) : col(kPaperDim), 20);
    return clicked;
}

bool App::slider(const char* id, float* value, float lowest, float highest, const char* format) {
    // ImGui's own slider takes the mouse and the keyboard, unseen.
    const ImVec4 clear(0, 0, 0, 0);
    for (const ImGuiCol part : {ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_Border,
                                ImGuiCol_SliderGrab, ImGuiCol_SliderGrabActive, ImGuiCol_Text})
        ImGui::PushStyleColor(part, clear);
    const bool changed = ImGui::SliderFloat(id, value, lowest, highest, format, ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopStyleColor(7);
    handCursor();
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const float share = std::clamp((*value - lowest) / (highest - lowest), 0.f, 1.f);
    const float fill = min.x + std::max(S(6), (max.x - min.x) * share);
    draw->AddRectFilled(min, max, col(hovered || active ? kSurfaceHover : kSurfaceHigh));
    draw->AddRectFilled(min, ImVec2(fill, max.y), col(hovered || active ? kGoldDeep : kGoldDrop));
    draw->AddLine(ImVec2(fill, min.y), ImVec2(fill, max.y), col(kGold), S(2));
    draw->AddRect(min, max, col(hovered || active ? kGoldDeep : kLine), 0, 0, S(1));
    char text[32];
    std::snprintf(text, sizeof(text), format, *value);
    const float size = ImGui::GetFontSize();
    const ImVec2 extent = measure(ImGui::GetFont(), size, text);
    draw->AddText(ImGui::GetFont(), size,
                  ImVec2(min.x + (max.x - min.x - extent.x) * 0.5f, min.y + (max.y - min.y - extent.y) * 0.5f),
                  col(kPaper), text);
    return changed;
}

bool App::combo(const char* id, int* index, const std::vector<std::string>& labels) {
    std::vector<const char*> items;
    for (const auto& label : labels)
        items.push_back(label.c_str());
    ImGui::PushFont(fonts_.semibold, 14);
    const bool changed = ImGui::Combo(id, index, items.data(), static_cast<int>(items.size()));
    handCursor();
    ImGui::PopFont();
    return changed;
}

bool App::link(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Text, vec(kGoldLift));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    const bool clicked = ImGui::IsItemClicked();
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, max.y), max, col(kGoldLift), 1);
    }
    return clicked;
}

float App::captionBox(ImDrawList* draw, ImVec2 at, float width, const std::string& text, Caption paper,
                      const std::string& emphasis) {
    // The narrator's words in italics beside a bar of the caption's colour.
    const Rgb bar = paper == Caption::alert ? kRed : paper == Caption::fact ? kFaint : kGold;
    const ImU32 accent = col(paper == Caption::alert ? kRedLift : kGoldLift);
    const float size = S(17), indent = S(18);
    const float wrap = width - indent;
    const float wordsHeight = text.empty() ? 0 : measure(fonts_.narration, size, text.c_str(), wrap).y;
    const float loudHeight = emphasis.empty() ? 0 : measure(fonts_.narration, size, emphasis.c_str(), wrap).y;
    const float height = wordsHeight + loudHeight + S(4);
    if (!draw)
        return height;
    draw->AddRectFilled(ImVec2(at.x, at.y + S(2)), ImVec2(at.x + S(3), at.y + height - S(1)), col(bar));
    draw->AddText(fonts_.narration, size, ImVec2(at.x + indent, at.y + S(1)), col(kPaper), text.c_str(), nullptr, wrap);
    if (!emphasis.empty())
        draw->AddText(fonts_.narration, size, ImVec2(at.x + indent, at.y + S(1) + wordsHeight), accent, emphasis.c_str(),
                      nullptr, wrap);
    return height;
}

float App::stamp(ImDrawList* draw, ImVec2 at, const char* text, ImU32 fill, ImU32 lettering, ImU32 keyline, float size,
                 ImU32 dot) {
    const std::string words = upper(text);
    const float spacing = 0.08f, dotWidth = dot ? size * 0.8f : 0;
    const ImVec2 pad(size * 0.7f, size * 0.3f);
    const float width = spacedWidth(fonts_.label, size, words.c_str(), spacing) + dotWidth + pad.x * 2;
    const ImVec2 min(at.x, at.y - size * 0.5f - pad.y), max(at.x + width, at.y + size * 0.5f + pad.y);
    draw->AddRectFilled(min, max, fill, S(3));
    draw->AddRect(min, max, keyline, S(3), 0, S(1));
    // A slow pulse, like a grace's light.
    if (dot) {
        const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.2f);
        const ImVec2 c(min.x + pad.x + size * 0.22f, at.y);
        glow(draw, c, size * 0.75f, kGold, 0.35f * pulse);
        draw->AddCircleFilled(c, size * 0.2f, dot, 12);
    }
    spacedText(draw, fonts_.label, size, ImVec2(min.x + pad.x + dotWidth, at.y - size * 0.54f), lettering,
               words.c_str(), spacing);
    return width;
}

float App::wordmark(ImDrawList* draw, ImVec2 at, float size) {
    engraved(draw, fonts_.display, size, at, col(kGold), "ERVR", 0.12f);
    return spacedWidth(fonts_.display, size, "ERVR", 0.12f);
}

// ERVR's mark: a gold ring holding two lenses (an image for each eye) and the sword between them. The window's
// icon is the same picture (tools/make_launcher_icon.py).
void App::emblem(ImDrawList* draw, ImVec2 c, float r) {
    const float line = std::max(1.2f, r * 0.075f);
    draw->AddCircleFilled(c, r, col(kSurfaceHigh), 64);
    glow(draw, c, r * 0.95f, kGold, 0.12f);
    draw->AddCircle(c, r - line * 0.5f, col(kGold), 64, line);
    draw->AddCircle(c, r * 0.8f, col(kGoldDeep), 64, std::max(1.f, line * 0.45f));
    const float lens = r * 0.24f, dx = r * 0.4f, ly = c.y + r * 0.04f;
    draw->AddCircle(ImVec2(c.x - dx, ly), lens, col(kGoldLift), 32, line);
    draw->AddCircle(ImVec2(c.x + dx, ly), lens, col(kGoldLift), 32, line);
    // The sword: pommel, grip, guard, blade to a point.
    const float top = c.y - r * 0.62f, guard = c.y - r * 0.36f, tip = c.y + r * 0.64f, blade = r * 0.075f;
    draw->AddCircleFilled(ImVec2(c.x, top), line * 0.9f, col(kGold), 12);
    draw->AddLine(ImVec2(c.x, top), ImVec2(c.x, guard), col(kGold), line);
    draw->AddLine(ImVec2(c.x - r * 0.2f, guard), ImVec2(c.x + r * 0.2f, guard), col(kGold), line * 1.1f);
    draw->AddQuadFilled(ImVec2(c.x - blade, guard), ImVec2(c.x + blade, guard), ImVec2(c.x + blade * 0.6f, tip - r * 0.14f),
                        ImVec2(c.x - blade * 0.6f, tip - r * 0.14f), col(kGoldLift));
    draw->AddTriangleFilled(ImVec2(c.x - blade * 0.6f, tip - r * 0.14f), ImVec2(c.x + blade * 0.6f, tip - r * 0.14f),
                            ImVec2(c.x, tip), col(kGoldLift));
}

// --- Layout ---------------------------------------------------------------------------------------------------------

void App::frame(float scale) {
    scale_ = scale;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##ervr", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    header();
    tabs();
    const float top = kHeaderHeight + kTabsHeight + 18;
    const ImVec2 origin(viewport->Pos.x + S(22), viewport->Pos.y + S(top));
    const ImVec2 size(viewport->Size.x - S(44), viewport->Size.y - S(top + 22));
    if (size.x > 0 && size.y > 0) {
        switch (tab_) {
        case Tab::play: play(origin, size); break;
        case Tab::options: options(origin, size); break;
        case Tab::controls: controls(origin, size); break;
        case Tab::about: about(origin, size); break;
        }
    }
    modals();
    ImGui::End();
    // An edit goes to ERVR.ini once the control is let go (a slider writes once, not every frame of the drag).
    if (iniDirty_ && !ImGui::IsAnyItemActive() && ImGui::GetTime() - iniEditedAt_ > 0.15)
        saveIni();
}

void App::header() {
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetWindowPos();
    const float width = ImGui::GetWindowWidth(), height = S(kHeaderHeight);
    // Light behind the mark, and rising into the top right corner.
    glow(draw, ImVec2(at.x + S(51), at.y + height * 0.5f), S(170), kGold, 0.09f);
    glow(draw, ImVec2(at.x + width, at.y), S(380), kGold, 0.05f);
    const ImVec2 center(at.x + S(24 + 27), at.y + height * 0.5f);
    emblem(draw, center, S(28));
    const float x = center.x + S(28 + 18);
    const float nameSize = S(42);
    const float nameWidth = wordmark(draw, ImVec2(x, at.y + S(11)), nameSize);
    // The release, and its price.
    {
        const float size = S(13.5f);
        const std::string version = format("V%s", kVersion);
        float tagX = x + nameWidth + S(18);
        const float tagY = at.y + S(36);
        tagX += stamp(draw, ImVec2(tagX, tagY), version.c_str(), col(kSurfaceHigh), col(kPaper), col(kLine), size) + S(8);
        stamp(draw, ImVec2(tagX, tagY), "Free \xC2\xB7 open source", col(kGoldDrop), col(kGoldLift), col(kGoldDeep), size);
    }
    const float kicker = S(13);
    const char* what = "NATIVE VR FOR ELDEN RING";
    const ImVec2 line(x + S(2), at.y + S(63));
    spacedText(draw, fonts_.kicker, kicker, line, col(kPaperDim), what, 0.14f);
    const std::string by = "  \xC2\xB7  BY " + upper(kAuthor);
    spacedText(draw, fonts_.kicker, kicker, ImVec2(line.x + spacedWidth(fonts_.kicker, kicker, what, 0.14f), line.y),
               col(kPaper), by.c_str(), 0.14f);

    // The tip jar keylined in gold, and where to ask for help: as wide as their words.
    const float buttonHeight = S(50);
    const auto stickerWidth = [&](const char* label, const char* icon) {
        const float labelSize = buttonHeight * 0.4f, iconSize = labelSize * 0.66f;
        const float iconWidth = icon && fonts_.icons ? measure(fonts_.label, iconSize, icon).x + labelSize * 0.36f : 0;
        return std::max(S(178), spacedWidth(fonts_.display, labelSize, upper(label).c_str(), 0.06f) + iconWidth + S(40));
    };
    const float kofiWidth = stickerWidth("Buy me a coffee", kHeart);
    float buttonX = at.x + width - S(26) - kofiWidth;
    const float buttonY = at.y + (height - buttonHeight) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(buttonX, buttonY));
    if (sticker("##kofi", kHeart, "Buy me a coffee", "on Ko-fi", ImVec2(kofiWidth, buttonHeight),
                {col(kSurfaceHigh), col(kSurfaceHover), col(kGoldLift), col(kGold), 0}))
        openUrl(kKofiUrl);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        tip("ERVR is free, every build, always. A tip keeps the updates coming; it unlocks nothing.");
    const float discordWidth = stickerWidth("Join the Discord", nullptr);
    buttonX -= discordWidth + S(14);
    ImGui::SetCursorScreenPos(ImVec2(buttonX, buttonY));
    if (sticker("##discord", nullptr, "Join the Discord", "News \xC2\xB7 help \xC2\xB7 clips",
                ImVec2(discordWidth, buttonHeight), {col(kSurfaceHigh), col(kSurfaceHover), col(kPaper), col(kLine), 0}))
        openUrl(kDiscordUrl);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        tip("Release news first, and help in #ervr-help: paste Copy report there with what happened.");
}

void App::tabs() {
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 window = ImGui::GetWindowPos();
    const float width = ImGui::GetWindowWidth(), y = window.y + S(kHeaderHeight), height = S(kTabsHeight);
    rule(draw, ImVec2(window.x, y + height - 1), width, kGoldDeep, 0.9f);
    float x = window.x + S(12);
    const std::pair<Tab, const char*> items[] = {
        {Tab::play, "Play"}, {Tab::options, "Options"}, {Tab::controls, "Controls"}, {Tab::about, "About"}};
    const float size = S(17), spacing = 0.1f;
    for (const auto& [tab, label] : items) {
        const std::string text = upper(label);
        const float textWidth = spacedWidth(fonts_.display, size, text.c_str(), spacing);
        const float itemWidth = textWidth + S(40);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ImGui::InvisibleButton(label, ImVec2(itemWidth, height)))
            tab_ = tab;
        handCursor();
        const bool active = tab_ == tab, hovered = ImGui::IsItemHovered();
        // The tab you are on: gold, over a rule with a diamond in its middle.
        if (active) {
            const float ruleY = y + height - S(9), middle = x + S(20) + textWidth * 0.5f;
            rule(draw, ImVec2(x + S(10), ruleY), itemWidth - S(20), kGold, 1, S(1.5f));
            diamond(draw, ImVec2(middle, ruleY + S(0.75f)), S(3.5f), col(kGoldLift));
        }
        spacedText(draw, fonts_.display, size, ImVec2(x + S(20), y + (height - size) * 0.5f - S(4)),
                   col(active ? kGoldLift : hovered ? kPaper : kPaperDim), text.c_str(), spacing);
        x += itemWidth;
    }
    // Right: the game running, as a pill whose light pulses.
    if (session_.running() || gameRunning_) {
        const char* label = session_.quitRequested()                                           ? "Closing the game"
                            : session_.running() && session_.phase() != Session::Phase::game ? "Starting"
                                                                                               : "In game";
        const float stampSize = S(14);
        const float stampWidth = spacedWidth(fonts_.label, stampSize, upper(label).c_str(), 0.08f) + stampSize * (0.8f + 1.4f);
        stamp(draw, ImVec2(window.x + width - S(28) - stampWidth, y + height * 0.5f - S(1)), label, col(kSurface),
              col(kGoldLift), col(kGoldDeep), stampSize, col(kGoldLift));
    }
}

void App::play(ImVec2 origin, ImVec2 size) {
    float top = origin.y;
    if (!settings_.shortcutsAsked && scan_.done) {
        ImGui::SetCursorScreenPos(origin);
        shortcutBanner(size.x);
        top += S(58) + S(18);
    }
    const float gap = S(20);
    const float panelWidth = std::clamp(size.x * 0.4f, S(380), S(470));
    const float rightWidth = size.x - panelWidth - gap;
    const float bottom = origin.y + size.y;
    // Left: VR itself.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, top));
    startPanel(ImVec2(panelWidth, bottom - top));
    // Right: this PC, then the session's messages.
    const float rightX = origin.x + panelWidth + gap;
    ImGui::SetCursorScreenPos(ImVec2(rightX, top));
    ImGui::BeginGroup();
    setupCard(rightWidth);
    ImGui::EndGroup();
    const float logTop = ImGui::GetItemRectMax().y + S(16);
    if (bottom - logTop > S(90)) {
        ImGui::SetCursorScreenPos(ImVec2(rightX, logTop));
        logCard(ImVec2(rightWidth, bottom - logTop));
    }
}

void App::startPanel(ImVec2 size) {
    const bool running = session_.running();
    const Readiness ready = readiness();
    const Progress progress = session_.progress();
    const auto phase = session_.phase();
    // How far the session got, on show after it ended too: 1 the headset, 2 the game took ERVR, 3 in VR.
    const int reached = progress.vr ? 3 : progress.loaded ? 2 : 1;
    enum class Step { pending, active, done, failed };
    Step steps[3] = {Step::pending, Step::pending, Step::pending};
    const auto advance = [&](Step state) {
        for (int i = 0; i < 3; ++i)
            steps[i] = i + 1 < reached ? Step::done : i + 1 == reached ? state : Step::pending;
    };
    // What is happening: a headline, the narrator's line (and a second, gold one), and the details.
    std::string headline, narration, aside, detail;
    Rgb tint = kPaper;
    Caption paper = Caption::narrator;
    bool offerHelp = false, offerCoffee = false, offerAdministrator = false;
    if (running) {
        advance(Step::active);
        const auto since = [](std::chrono::steady_clock::time_point t) {
            return static_cast<int>(std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count());
        };
        const int elapsed = since(session_.started());
        const bool gameUp = phase == Session::Phase::game;
        if (session_.quitRequested()) {
            headline = "Closing the game";
            narration = "ELDEN RING saves as it quits.";
            tint = kAmber;
        } else if (phase == Session::Phase::steam) {
            headline = "Starting Steam";
            narration = "The game needs Steam running and signed in. Sign in if it asks.";
        } else if (progress.disabled) {
            headline = "ERVR is off";
            narration = "ERVR.ini says Enabled=false, so the game runs flat.";
            tint = kAmber;
        } else if (progress.vr) {
            headline = format("In VR  \xC2\xB7  %d:%02d", elapsed / 60, elapsed % 60);
            narration = "Change the options while you play: ERVR takes them within a second.";
            tint = kGoldLift;
        } else if (progress.waiting) {
            headline = "Waiting for the headset";
            narration = "The VR runtime does not take connections yet. ERVR asks again every 2 seconds.";
            aside = "Virtual Desktop: connect and stream, then wait.";
            tint = kAmber;
        } else if (progress.loaded) {
            headline = "The game is starting";
            narration = "Put the headset on: the title screen shows on a big screen there.";
        } else if (gameUp && gameSeenAt_ != std::chrono::steady_clock::time_point{} && since(gameSeenAt_) > 45) {
            headline = "ERVR did not load";
            narration = "The game runs, but ERVR wrote no log. Is it installed? See ERVR under This PC.";
            paper = Caption::alert;
            tint = kAmber;
        } else {
            headline = "Starting ELDEN RING";
            narration = "Put the headset on and keep it connected.";
        }
    } else if (auto code = session_.exitCode()) {
        for (auto it = log_.rbegin(); it != log_.rend() && detail.empty(); ++it)
            if (it->kind == LineKind::error)
                detail = it->text;
        if (*code == 0) {
            advance(Step::done);
            headline = "Session ended";
            narration = "ERVR's log stays in the game folder, as ERVR\\ERVR.log.";
            if (reached == 3) {
                aside = "Had a good run? A coffee keeps the updates coming.";
                offerCoffee = true;
            }
            detail.clear();
        } else if (*code == 130) {
            advance(Step::pending);
            headline = "Start cancelled";
            narration = "Press START VR when you are ready.";
            tint = kPaperDim;
        } else {
            advance(Step::failed);
            headline = *code == 2 ? "Could not start" : "The game stopped";
            narration = "The log says why. Copy a report and ask on GitHub.";
            paper = Caption::alert;
            offerHelp = true;
        }
    } else if (!startError_.empty()) {
        headline = "Could not start";
        narration = "Nothing was started. What went wrong is below.";
        paper = Caption::alert;
        detail = startError_;
        offerHelp = true;
    } else if (!ready.blockers.empty()) {
        headline = ready.checking ? "Checking this PC" : "Not ready yet";
        narration = ready.checking ? "The launcher looks for the game, your VR runtime and ERVR's files."
                                   : "Fix what This PC marks in red. Then press START VR.";
        if (!ready.checking)
            detail = join(ready.blockers, " ");
        tint = ready.checking ? kPaper : kAmber;
        offerAdministrator = !ready.checking && (ready.needsInstall || ready.needsSwitch) && !scan_.writable;
    } else {
        headline = "Ready";
        narration = "Connect your headset. Then press START VR.";
        if (ready.needsSwitch)
            aside = "START VR first parks the flat mods in VR's way.";
        else if (ready.needsInstall)
            aside = scan_.install.state == InstallState::older ? "START VR first updates ERVR to " + scan_.payloadVersion + "."
                                                              : "START VR first installs ERVR into the game folder.";
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(kSurface));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(kLine));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(22), S(20)));
    const bool open = ImGui::BeginChild("##start", size, ImGuiChildFlags_Borders,
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    if (!open) {
        ImGui::EndChild();
        return;
    }
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetWindowPos(), max = add(min, ImGui::GetWindowSize());
    const float padX = S(22), padY = S(20);
    // Words and buttons in front; the light goes behind them.
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(0);
    const float lineHeight = S(24);
    const float lineY = max.y - padY - lineHeight;
    const float buttonHeight = S(70);
    const float buttonY = lineY - S(16) - buttonHeight;
    glow(draw, ImVec2(min.x + (max.x - min.x) * 0.5f, buttonY + buttonHeight * 0.5f), (max.x - min.x) * 0.75f,
         running ? kGoldLift : kGold, running ? 0.10f : 0.07f);
    corners(draw, min, max, S(12), col(kGoldDeep), S(1.5f));
    draw->ChannelsSetCurrent(1);
    const float left = min.x + padX, innerWidth = max.x - min.x - padX * 2;
    float y = min.y + padY - S(4);
    // The headline, as large as fits.
    const std::string head = upper(headline);
    float headSize = S(46);
    while (headSize > S(24) && spacedWidth(fonts_.display, headSize, head.c_str(), 0.03f) > innerWidth)
        headSize -= S(1);
    engraved(draw, fonts_.display, headSize, ImVec2(left, y), col(tint), head.c_str(), 0.03f);
    y += headSize * 1.08f;
    rule(draw, ImVec2(left - S(6), y), innerWidth * 0.8f, kGoldDeep, 0.9f);
    y += S(12);
    y += captionBox(draw, ImVec2(left, y), innerWidth - S(8), narration, paper, aside) + S(18);

    struct StepWords {
        const char* title;
        const char* text;
    };
    static constexpr StepWords words[] = {
        {"Headset", "Connect it to this PC: Quest through Link, Air Link or Virtual Desktop (streaming), or start SteamVR."},
        {"Game", "The launcher starts ELDEN RING with EAC off. Its title screen shows on a big screen in the headset."},
        {"Play", "Load your save: full stereo takes over in the world. Menus and loading screens use the big screen."},
    };
    const float badgeSize = S(26), titleSize = S(17), textSize = S(14);
    const float textX = left + badgeSize + S(14), textWidth = innerWidth - badgeSize - S(14);
    float heights[3], full = S(14) * 2;
    for (int i = 0; i < 3; ++i) {
        heights[i] = std::max(badgeSize, titleSize + S(2) + measure(fonts_.body, textSize, words[i].text, textWidth).y);
        full += heights[i];
    }
    // The details in as many whole lines as there is room for: first the steps' words give way, then the steps.
    const bool offers = offerHelp || offerCoffee || offerAdministrator;
    const float offersHeight = offers ? ImGui::GetFrameHeight() + S(18) : 0;
    const float room = buttonY - S(24) - y - offersHeight, compact = badgeSize * 3 + S(10) * 2;
    const float detailSize = S(14.5f);
    const float detailFull = detail.empty() ? 0 : measure(fonts_.body, detailSize, detail.c_str(), innerWidth).y + S(16);
    const float detailNeed = std::min(detailFull, detailSize * 5 + S(16));
    const bool spoken = full + detailNeed <= room, shown = spoken || compact + detailNeed <= room;
    const float stepsHeight = spoken ? full : shown ? compact : 0;
    const float lines = std::floor((std::min(detailFull, room - stepsHeight) - S(16)) / detailSize);
    if (lines > 0) {
        draw->PushClipRect(ImVec2(left, y), ImVec2(left + innerWidth, y + lines * detailSize), true);
        draw->AddText(fonts_.body, detailSize, ImVec2(left, y), col(kPaper), detail.c_str(), nullptr, innerWidth);
        draw->PopClipRect();
        y += lines * detailSize + S(16);
    }
    if (offers) {
        ImGui::SetCursorScreenPos(ImVec2(left, y));
        bool first = true;
        const auto offer = [&](const char* label, Ink ink, const char* icon) {
            if (!first)
                ImGui::SameLine(0, S(10));
            first = false;
            return button(label, buttonWidth(label, icon), ink, icon);
        };
        if (offerAdministrator && offer("Restart as administrator", Ink::primary, nullptr)) {
            std::string error;
            if (restartAsAdministrator(error))
                quit_ = true;
            else
                startError_ = error;
        }
        if (offerHelp) {
            if (offer("Copy report", Ink::plain, kCopy)) {
                ImGui::SetClipboardText(report().c_str());
                reportCopied_ = ImGui::GetTime();
            }
            if (offer("Open an issue", Ink::plain, nullptr))
                openUrl(kIssuesUrl);
        }
        if (offerCoffee && offer("Buy me a coffee", Ink::support, kHeart))
            openUrl(kKofiUrl);
        y += offersHeight;
    }
    float stepY = y;
    for (int i = 0; i < (shown ? 3 : 0); ++i) {
        const Step state = steps[i];
        const float height = spoken ? heights[i] : badgeSize, gap = spoken ? S(14) : S(10);
        const ImVec2 c(left + badgeSize * 0.5f, stepY + badgeSize * 0.5f);
        const float r = badgeSize * 0.5f;
        // The thread down to the next step.
        if (i < 2)
            draw->AddLine(ImVec2(c.x, c.y + r), ImVec2(c.x, stepY + height + gap), col(state == Step::done ? kGreen : kLine),
                          S(1.5f));
        // A diamond in the step's colour: outlined for what is to come, filled only for a fault.
        const Rgb edge = state == Step::done     ? kGreen
                         : state == Step::active ? kGold
                         : state == Step::failed ? kRed
                                                 : kLine;
        diamond(draw, c, r, col(state == Step::failed ? kRed : kSurfaceHigh));
        diamond(draw, c, r, col(edge), false, S(1.8f));
        const float w = S(2.2f);
        if (state == Step::done) {
            draw->PathLineTo(ImVec2(c.x - r * 0.36f, c.y + r * 0.02f));
            draw->PathLineTo(ImVec2(c.x - r * 0.08f, c.y + r * 0.3f));
            draw->PathLineTo(ImVec2(c.x + r * 0.4f, c.y - r * 0.28f));
            draw->PathStroke(col(kGreen), 0, w);
        } else if (state == Step::failed) {
            draw->AddLine(ImVec2(c.x - r * 0.28f, c.y - r * 0.28f), ImVec2(c.x + r * 0.28f, c.y + r * 0.28f), col(kWhite), w);
            draw->AddLine(ImVec2(c.x + r * 0.28f, c.y - r * 0.28f), ImVec2(c.x - r * 0.28f, c.y + r * 0.28f), col(kWhite), w);
        } else {
            const char number[2] = {static_cast<char>('1' + i), 0};
            const float numberSize = S(14);
            const ImVec2 extent = measure(fonts_.display, numberSize, number);
            draw->AddText(fonts_.display, numberSize, ImVec2(c.x - extent.x * 0.5f, c.y - extent.y * 0.52f),
                          col(state == Step::active ? kGoldLift : kPaperDim), number);
        }
        const ImU32 titleColor = col(state == Step::pending ? kPaperDim : state == Step::active ? kGoldLift : kPaper);
        const float titleY = spoken ? stepY - S(2) : c.y - titleSize * 0.54f;
        spacedText(draw, fonts_.label, titleSize, ImVec2(textX, titleY), titleColor, upper(words[i].title).c_str(), 0.06f);
        if (spoken)
            draw->AddText(fonts_.body, textSize, ImVec2(textX, stepY + titleSize + S(1)), col(kPaperDim), words[i].text,
                          nullptr, textWidth);
        stepY += height + gap;
    }

    // START VR, the one gold thing, on a dark drop; QUIT GAME keylined in gold.
    ImGui::SetCursorScreenPos(ImVec2(left, buttonY));
    const float stickerDrop = std::round(std::max(S(4), buttonHeight * 0.07f));
    const ImVec2 buttonSize(innerWidth - stickerDrop, buttonHeight);
    const Paint quiet{col(kSurfaceHigh), col(kSurfaceHover), col(kPaper), col(kGoldDeep), 0};
    if (!running) {
        if (sticker("##start", kPlay, "Start VR", nullptr, buttonSize,
                    {col(kGold), col(kGoldLift), col(kInk), col(kGoldLift), col(kGoldDrop)}, ready.blockers.empty()))
            start();
        if (!ready.blockers.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            tip(join(ready.blockers, "\n").c_str());
    } else if (phase != Session::Phase::game) {
        if (sticker("##cancel", kStop, "Cancel", nullptr, buttonSize, quiet))
            session_.requestQuit();
    } else if (!session_.quitRequested()) {
        if (sticker("##quit", kStop, "Quit game", nullptr, buttonSize, quiet))
            session_.requestQuit();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            tip("Closes ELDEN RING as Alt+F4 does. The game saves as it goes, and again as it quits.");
    } else if (std::chrono::steady_clock::now() - session_.quitRequestedAt() > std::chrono::seconds(15)) {
        if (sticker("##force", kStop, "Force close", nullptr, buttonSize,
                    {col(kSurfaceHigh), col(kSurfaceHover), col(kRedLift), col(kRed), 0}))
            session_.forceQuit();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            tip("Ends the game's process at once, without waiting for it to quit.");
    } else {
        sticker("##closing", nullptr, "Closing...", nullptr, buttonSize, quiet, false);
    }

    // How the game starts, and the choice when Seamless Co-op is there too.
    spacedText(draw, fonts_.kicker, S(13), ImVec2(left, lineY + S(5)), col(kPaperDim), "START WITH", 0.14f);
    const float choiceX = left + spacedWidth(fonts_.kicker, S(13), "START WITH", 0.14f) + S(14);
    if (scan_.coop) {
        ImGui::SetCursorScreenPos(ImVec2(choiceX, lineY - S(3)));
        ImGui::BeginDisabled(running);
        ImGui::SetNextItemWidth(std::min(S(250), left + innerWidth - choiceX));
        int mode = settings_.mode == StartMode::coop ? 1 : 0;
        if (combo("##mode", &mode, {"Offline, EAC off", "Seamless Co-op"})) {
            settings_.mode = mode ? StartMode::coop : StartMode::offline;
            save();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            tip("Offline: eldenring.exe with steam_appid.txt beside it, no EAC. Seamless Co-op: its own "
                "ersc_launcher.exe starts the game with EAC off; your partners don't need ERVR.");
    } else {
        draw->AddText(fonts_.semibold, S(15), ImVec2(choiceX, lineY + S(2)), col(kPaper), "Offline, EAC off");
    }
    draw->ChannelsMerge();
    ImGui::EndChild();
}

void App::shortcutBanner(float width) {
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = S(58);
    draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), col(kSurface));
    draw->AddRect(at, ImVec2(at.x + width, at.y + height), col(kLine));
    corners(draw, at, ImVec2(at.x + width, at.y + height), S(9), col(kGoldDeep), S(1.5f));
    diamond(draw, ImVec2(at.x + S(24), at.y + height * 0.5f), S(5), col(kGold));
    const char* message = shortcutMessage_.empty()
                              ? "Start ERVR from your desktop and Start menu: add shortcuts to this launcher."
                              : shortcutMessage_.c_str();
    const float buttonW = S(150), textX = at.x + S(42);
    const float textWidth = width - (textX - at.x) - buttonW * 2 - S(40);
    const ImVec2 textSize = measure(fonts_.semibold, S(15), message, textWidth);
    draw->AddText(fonts_.semibold, S(15), ImVec2(textX, at.y + (height - textSize.y) * 0.5f), col(kPaper), message, nullptr,
                  textWidth);
    ImGui::SetCursorScreenPos(
        ImVec2(at.x + width - S(16) - buttonW * 2 - S(10), at.y + (height - ImGui::GetFrameHeight()) * 0.5f));
    if (button("Add shortcuts", buttonW, Ink::primary, kShortcut)) {
        std::string error;
        shortcutMessage_ = createShortcuts(error) ? "" : error;
        settings_.shortcutsAsked = true;
        save();
    }
    ImGui::SameLine(0, S(10));
    if (button("Not now", buttonW)) {
        settings_.shortcutsAsked = true;
        save();
    }
}

void App::setupRow(Mark mark, const char* title, const std::string& detail, float actionWidth,
                   const std::function<void()>& action, bool last) {
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float textX = at.x + S(36);
    const float textWidth = width - S(36) - (actionWidth > 0 ? actionWidth + S(16) : 0);
    const std::string name = upper(title);
    const float titleSize = S(16.5f), detailSize = S(14);
    const ImVec2 detailExtent = measure(fonts_.body, detailSize, detail.c_str(), textWidth);
    const float height = std::max(S(54), S(10) + titleSize + S(2) + detailExtent.y + S(10));
    statusMark(draw, ImVec2(at.x + S(11), at.y + S(10) + titleSize * 0.5f), mark);
    spacedText(draw, fonts_.label, titleSize, ImVec2(textX, at.y + S(9)), col(kPaper), name.c_str(), 0.05f);
    draw->AddText(fonts_.body, detailSize, ImVec2(textX, at.y + S(10) + titleSize + S(2)), col(kPaperDim), detail.c_str(),
                  nullptr, textWidth);
    if (action) {
        ImGui::SetCursorScreenPos(ImVec2(at.x + width - actionWidth, at.y + (height - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::PushID(title);
        ImGui::PushItemWidth(actionWidth);
        action();
        ImGui::PopItemWidth();
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + height));
    ImGui::Dummy(ImVec2(width, 0));
    if (!last)
        draw->AddLine(ImVec2(textX, at.y + height - S(0.5f)), ImVec2(at.x + width, at.y + height - S(0.5f)), col(kLine));
}

void App::setupCard(float width) {
    if (!beginCard("##setup", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY))
        return endCard();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(10), 0));
    const float actionWidth = S(130);
    cardTitle("This PC", actionWidth);
    ImGui::BeginDisabled(scanner_.busy());
    if (button("Re-check", actionWidth, Ink::plain, kRefresh))
        rescan();
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, S(4)));
    const bool running = session_.running();
    const bool changing = running || gameRunning_;
    const Install& in = scan_.install;

    // The game.
    {
        Mark mark = Mark::busy;
        std::string detail = "Looking for ELDEN RING...";
        const std::string path = narrow(scan_.gameDir);
        if (scan_.done && scan_.gameExe.empty()) {
            mark = Mark::error;
            detail = "Not found in your Steam libraries. Show the launcher where Game\\eldenring.exe is.";
        } else if (scan_.done && !scan_.gamePatch.empty()) {
            mark = Mark::ok;
            detail = "Patch " + scan_.gamePatch + " (exe " + scan_.gameVersion + "): ERVR's address map is verified" +
                     (gameRunning_ ? ". Running now" : "") + "\n" + path;
        } else if (scan_.done) {
            mark = Mark::warning;
            detail = "Exe " + (scan_.gameVersion.empty() ? std::string("of unknown version") : scan_.gameVersion) +
                     ": ERVR has no verified address map for it. It still loads; what it cannot find stays off "
                     "(the log names it).\n" +
                     path;
        }
        setupRow(mark, "ELDEN RING", detail, actionWidth, [&] {
            ImGui::BeginDisabled(changing);
            if (button("Change...", actionWidth))
                if (auto chosen = browseForGame(window_, scan_.gameExe)) {
                    settings_.gameExe = *chosen;
                    save();
                    rescan();
                }
            ImGui::EndDisabled();
        });
    }

    // The VR runtime.
    {
        Mark mark = Mark::busy;
        std::string detail = "Looking for OpenXR runtimes...";
        if (scan_.done && scan_.runtimes.empty()) {
            mark = Mark::error;
            detail = "None installed. Install Meta Quest Link, Virtual Desktop's streamer or SteamVR.";
        } else if (scan_.done) {
            mark = Mark::ok;
            detail = settings_.runtime == kAutoRuntime
                         ? "Automatic: Virtual Desktop while its streamer runs, SteamVR while it runs, else Windows' "
                           "active runtime. Now: " + runtimeWords() + "."
                         : "ERVR uses " + runtimeWords() + " for its sessions, whatever Windows' active runtime is.";
        }
        const float comboWidth = S(220);
        setupRow(mark, "VR runtime", detail, comboWidth, [&] {
            std::vector<std::string> labels{"Automatic"};
            int index = 0;
            for (size_t i = 0; i < scan_.runtimes.size(); ++i) {
                const Runtime& r = scan_.runtimes[i];
                labels.push_back(r.name + (r.active ? " (active)" : ""));
                if (_wcsicmp(r.manifest.c_str(), settings_.runtime.c_str()) == 0)
                    index = static_cast<int>(i) + 1;
            }
            ImGui::BeginDisabled(running || scan_.runtimes.empty());
            ImGui::SetNextItemWidth(comboWidth);
            if (combo("##runtime", &index, labels)) {
                settings_.runtime = index == 0 ? std::wstring(kAutoRuntime) : scan_.runtimes[index - 1].manifest;
                headset_.reset();
                save();
            }
            ImGui::EndDisabled();
        });
    }

    // The headset.
    {
        Mark mark = Mark::info;
        std::string detail = "Connect it to this PC, then check it here. Optional: ERVR waits for it anyway.";
        const auto state = headset_.state();
        const auto result = headset_.result();
        if (state == Outcome::running) {
            mark = Mark::busy;
            detail = "Asking " + runtimeWords() + " for a headset... (SteamVR may take half a minute to start)";
        } else if (state == Outcome::ok) {
            mark = Mark::ok;
            detail = (result.headset.empty() ? std::string("A headset") : result.headset) + " answered through " +
                     (result.runtime.empty() ? runtimeWords() : result.runtime) +
                     (result.eye.empty() ? "." : ": " + result.eye + " per eye.");
        } else if (state == Outcome::error) {
            mark = Mark::error;
            detail = "No headset answered: " + (result.error.empty() ? std::string("unknown reason") : result.error) + ".";
        }
        setupRow(mark, "Headset", detail, actionWidth, [&] {
            ImGui::BeginDisabled(state == Outcome::running || scan_.runtimes.empty() || changing);
            if (button("Check", actionWidth)) {
                const Runtime* r = sessionRuntime();
                headset_.start(r ? r->manifest : std::wstring());
            }
            ImGui::EndDisabled();
            if (changing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                tip("The game holds the headset while it runs.");
        });
    }

    // ERVR in the game folder.
    {
        Mark mark = Mark::busy;
        std::string detail = "Checking...";
        const char* label = "Open folder";
        std::function<void()> act = [&] { openPath((std::filesystem::path(scan_.gameDir) / L"ERVR").wstring()); };
        const bool payload = !scan_.payload.empty() && scan_.payloadMissing.empty();
        const std::string carried = scan_.payloadVersion.empty() ? "" : scan_.payloadVersion;
        if (scan_.done && scan_.gameDir.empty()) {
            mark = Mark::info;
            detail = "Waiting for the game's folder.";
            act = nullptr;
        } else if (scan_.done) {
            switch (in.state) {
            case InstallState::missing:
                mark = payload ? Mark::info : Mark::error;
                detail = payload ? "Not in the game folder yet: START VR installs ERVR " + carried +
                                       " (ReShade with add-on support as dxgi.dll, dinput8.dll, the ERVR folder)."
                                 : "Not installed, and this folder lacks ERVR's files: extract the whole zip again." +
                                       (scan_.payloadMissing.empty() ? std::string()
                                                                     : " Missing: " + join(scan_.payloadMissing, ", "));
                label = "Install";
                act = payload ? std::function<void()>([&] { apply(installErvr(scan_), "Installing ERVR"); }) : nullptr;
                break;
            case InstallState::current:
                mark = Mark::ok;
                detail = "ERVR " + in.version + " installed" + (scan_.payloadInGame ? " (the launcher runs from it)" : "") +
                         ". Settings: ERVR\\ERVR.ini, log: ERVR\\ERVR.log.";
                break;
            case InstallState::older:
                mark = Mark::warning;
                detail = "ERVR " + in.version + " installed: START VR updates it to " + carried + " (your ERVR.ini stays).";
                label = "Update";
                act = [&] { apply(installErvr(scan_), "Updating ERVR"); };
                break;
            case InstallState::newer:
                mark = Mark::info;
                detail = "ERVR " + in.version + " installed, newer than this launcher's " + carried + ": left as it is.";
                break;
            case InstallState::different:
                mark = Mark::info;
                detail = "ERVR " + in.version + " installed, another build of the same version than this launcher's.";
                label = "Replace";
                act = [&] { apply(installErvr(scan_, true), "Replacing ERVR"); };
                break;
            case InstallState::parked:
                mark = Mark::warning;
                detail = "Parked for flat play (dinput8.dll.ervr-off): START VR switches the game folder back to VR.";
                label = "Switch to VR";
                act = [&] { apply(switchToVr(scan_), "Switching to VR"); };
                break;
            }
            if (in.foreignProxy) {
                mark = Mark::warning;
                detail += " Another dinput8.dll mod loader is in the game folder: ERVR only loads if that loader "
                          "loads ERVR\\ERVR.dll (put it in its mods folder).";
            }
            if (!in.reshade && in.state != InstallState::missing && in.state != InstallState::parked) {
                mark = mark == Mark::ok ? Mark::warning : mark;
                detail += in.dxgi.empty() ? " dxgi.dll (ReShade) is missing: installing puts it back."
                                          : " dxgi.dll is " + in.dxgi + ": installing puts the bundled ReShade in its "
                                                                        "place (the old one kept as dxgi.dll.ervr-bak).";
                if (payload && label == std::string("Open folder")) {
                    label = "Install";
                    act = [&] { apply(installErvr(scan_), "Installing ReShade"); };
                }
            }
        }
        setupRow(mark, "ERVR", detail, actionWidth, [&] {
            ImGui::BeginDisabled(!act || (changing && label != std::string("Open folder")));
            if (button(label, actionWidth) && act)
                act();
            ImGui::EndDisabled();
        });
    }

    // The game's own graphics settings.
    {
        const GameGraphics& graphics = scan_.graphics;
        std::vector<std::string> chosen, on;
        for (size_t i = 0; i < text::kVrEffects.size(); ++i) {
            if (!settings_.effectsOff[i])
                continue;
            chosen.push_back(text::kVrEffects[i].name);
            if (!graphics.values[i].empty() && !text::sameText(graphics.values[i], text::kEffectOff))
                on.push_back(std::string(text::kVrEffects[i].name) + " (" + effectWord(graphics.values[i]) + ")");
        }
        Mark mark = Mark::busy;
        std::string detail = "Checking...";
        if (scan_.done && chosen.empty()) {
            mark = Mark::info;
            detail = "Left as the game's own menu sets them (Options > Game graphics).";
        } else if (scan_.done && graphics.files.empty()) {
            mark = Mark::info;
            detail = "ELDEN RING writes its graphics settings on its first start: from the next one, START VR turns "
                     "off " + text::listed(chosen) + ".";
        } else if (scan_.done && on.empty()) {
            mark = Mark::ok;
            detail = "Off in the game's own settings: " + text::listed(chosen) + ".";
        } else if (scan_.done) {
            mark = Mark::info;
            detail = "On in the game's own settings: " + text::listed(on) + ". START VR turns " +
                     (on.size() > 1 ? "them" : "it") + " off (Options > Game graphics).";
        }
        const bool turnOff = !on.empty();
        setupRow(mark, "Game graphics", detail, actionWidth, [&] {
            ImGui::BeginDisabled(!scan_.done || (turnOff && changing));
            if (button(turnOff ? "Turn off" : "Options", actionWidth)) {
                if (turnOff)
                    turnOffEffects();
                else
                    tab_ = Tab::options;
            }
            ImGui::EndDisabled();
            if (turnOff && changing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                tip("ELDEN RING writes its settings back as it closes: START VR turns these off next time.");
        });
    }

    // What else is in the game folder.
    {
        Mark mark = Mark::busy;
        std::string detail = "Checking...";
        std::vector<std::string> live, parked;
        for (const auto& mod : in.flat)
            (mod.parked ? parked : live).push_back(mod.label);
        if (scan_.done && scan_.gameDir.empty()) {
            mark = Mark::info;
            detail = "Waiting for the game's folder.";
        } else if (scan_.done && live.empty()) {
            mark = Mark::ok;
            detail = parked.empty() ? "Nothing in VR's way: no flat render mods, no R.E.A.L. VR."
                                    : "Parked for VR (renamed .ervr-off): " + join(parked, ", ") +
                                          ". About > Play flat puts them back.";
        } else if (scan_.done) {
            mark = Mark::warning;
            detail = "In VR's way: " + join(live, ", ") +
                     ". They break the headset image (HDR re-encoding, DXGI wrapping, effects once per eye): START VR "
                     "parks them (renamed .ervr-off; nothing is deleted).";
        }
        setupRow(mark, "Other mods", detail, actionWidth, [&] {
            ImGui::BeginDisabled(live.empty() || changing);
            if (button("Park now", actionWidth))
                apply(switchToVr(scan_), "Parking the flat mods");
            ImGui::EndDisabled();
        }, true);
    }
    ImGui::PopStyleVar();
    endCard();
}

void App::logCard(ImVec2 size) {
    if (!beginCard("##log", size)) {
        endCard();
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), 0));
    const float copyWidth = S(96), openWidth = S(124), reportWidth = S(160);
    cardTitle("Session log", copyWidth + S(8) + openWidth + S(8) + reportWidth);
    ImGui::BeginDisabled(log_.empty());
    if (button("Copy", copyWidth, Ink::plain, kCopy))
        copyLog();
    ImGui::EndDisabled();
    ImGui::SameLine(0, S(8));
    const std::wstring logFile = (std::filesystem::path(scan_.gameDir) / L"ERVR" / L"ERVR.log").wstring();
    ImGui::BeginDisabled(scan_.gameDir.empty());
    if (button("Open log", openWidth, Ink::plain, kDocument))
        openPath(logFile);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        tip("ERVR\\ERVR.log in the game folder; the two sessions before it are ERVR.log.prev and .prev2.");
    ImGui::SameLine(0, S(8));
    // For a bug report: what a helper asks first, ready to paste. The button says so for two seconds.
    const bool copied = ImGui::GetTime() - reportCopied_ < 2;
    if (button(copied ? "Copied###report" : "Copy report###report", reportWidth, Ink::plain, kCopy)) {
        ImGui::SetClipboardText(report().c_str());
        reportCopied_ = ImGui::GetTime();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        tip("Copies ERVR's version, the game, your runtime and headset, your PC and the last session in a few lines: "
            "paste them into a GitHub issue, with ERVR.log attached.");
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0, S(8)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(kWell));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(kLine));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(12)));
    ImGui::BeginChild("##lines", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (log_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, vec(kPaperDim));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(8)));
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("What the session does shows here, line by line: Steam, the game, then ERVR's own log as "
                               "it writes it.");
        ImGui::TextUnformatted("Something went wrong? Copy report and open an issue on GitHub, with ERVR.log attached "
                               "(LogLevel debug in Options > Troubleshooting gives it the detail a fix needs).");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    } else {
        ImGui::PushFont(fonts_.mono, 13);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(3)));
        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - S(4);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(log_.size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& line = log_[i];
                const Rgb tint = line.kind == LineKind::error     ? kRedLift
                                 : line.kind == LineKind::warning ? kAmber
                                 : line.kind == LineKind::good    ? kGreen
                                 : line.kind == LineKind::muted   ? kFaint
                                                                  : kLogText;
                if (!line.time.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, vec(kFaint));
                    ImGui::TextUnformatted(line.time.c_str());
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0, S(10));
                }
                ImGui::PushStyleColor(ImGuiCol_Text, vec(tint));
                ImGui::TextUnformatted(line.text.c_str());
                ImGui::PopStyleColor();
            }
        if (scrollLog_ && atBottom)
            ImGui::SetScrollHereY(1.0f);
        scrollLog_ = false;
        ImGui::PopStyleVar();
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    endCard();
}

void App::option(const char* title, const char* help, float controlWidth, const std::function<void()>& control,
                 bool warning) {
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    // A rule between rows, none above the card's first.
    if (!firstOption_)
        draw->AddLine(start, ImVec2(start.x + width, start.y), col(kLine));
    const ImVec2 at(start.x, start.y + (firstOption_ ? S(4) : S(12)));
    firstOption_ = false;
    const float textWidth = width - controlWidth - S(16);
    const ImVec2 titleSize = measure(fonts_.semibold, S(15.5f), title, textWidth);
    const ImVec2 helpSize = measure(fonts_.body, S(13.5f), help, textWidth);
    draw->AddText(fonts_.semibold, S(15.5f), at, col(kPaper), title, nullptr, textWidth);
    draw->AddText(fonts_.body, S(13.5f), ImVec2(at.x, at.y + titleSize.y + S(2)), col(warning ? kAmber : kPaperDim), help,
                  nullptr, textWidth);
    const float height = std::max(titleSize.y + S(2) + helpSize.y, ImGui::GetFrameHeight());
    ImGui::SetCursorScreenPos(
        ImVec2(at.x + width - controlWidth, at.y + std::max(0.f, (titleSize.y - ImGui::GetFrameHeight()) * 0.5f) - S(1)));
    ImGui::PushID(title);
    ImGui::PushItemWidth(controlWidth);
    control();
    ImGui::PopItemWidth();
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(start.x, at.y + height + S(12)));
    ImGui::Dummy(ImVec2(width, 0));
}

void App::options(ImVec2 origin, ImVec2 size) {
    ImGui::SetCursorScreenPos(origin);
    ImGui::BeginChild("##optionsScroll", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    const ImVec2 base = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize - S(6);
    const float gap = S(18);
    float top = base.y;
    auto* draw = ImGui::GetWindowDrawList();
    // Where the options go, and when they apply.
    {
        std::string words, loud;
        Caption paper = Caption::narrator;
        const bool inGame = ini_ && !scan_.gameDir.empty() &&
                            _wcsicmp(ini_->path.c_str(),
                                     (std::filesystem::path(scan_.gameDir) / L"ERVR\\ERVR.ini").wstring().c_str()) == 0;
        if (!ini_) {
            words = "ERVR.ini was not found. Install ERVR first (Play tab), or extract the whole zip again.";
            paper = Caption::alert;
        } else if (!inGame) {
            words = "ERVR is not in the game folder yet: these are the settings it will be installed with.";
            paper = Caption::fact;
        } else {
            words = "Saved to ERVR.ini in the game folder. ERVR takes a change within a second, even mid-game.";
            if (gameRunning_)
                loud = "The game is running: try them now.";
        }
        if (!iniError_.empty()) {
            words = iniError_;
            paper = Caption::alert;
        }
        const float buttonsWidth = buttonWidth("Open ERVR.ini", kDocument) + S(10) + buttonWidth("Defaults", kRefresh);
        const float captionWidth = width - buttonsWidth - S(24);
        const float height = std::max(captionBox(nullptr, base, captionWidth, words, paper, loud), ImGui::GetFrameHeight());
        captionBox(draw, ImVec2(base.x, top + S(2)), captionWidth, words, paper, loud);
        ImGui::SetCursorScreenPos(ImVec2(base.x + width - buttonsWidth, top));
        ImGui::BeginDisabled(!ini_);
        if (button("Open ERVR.ini", buttonWidth("Open ERVR.ini", kDocument), Ink::plain, kDocument))
            openPath(ini_->path);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            tip("Every setting is there, each with what it does. This tab has the ones players change most.");
        ImGui::SameLine(0, S(10));
        ImGui::BeginDisabled(!inGame || scan_.payload.empty() || scan_.payloadInGame);
        if (button("Defaults", buttonWidth("Defaults", kRefresh), Ink::plain, kRefresh))
            ask_ = Ask::defaults;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
            tip("Puts this launcher's ERVR.ini in place of the game folder's; yours is kept beside it.");
        ImGui::EndDisabled();
        top += height + gap;
    }
    ImGui::BeginDisabled(!ini_);

    // Controls bound to ERVR.ini keys.
    const auto onOff = [&](const char* id, const char* section, const char* key, bool fallback) {
        bool on = truthy(iniValue(section, key, fallback ? "true" : "false"));
        if (toggle(id, &on))
            setIni(section, key, on ? "true" : "false");
    };
    const auto choice = [&](const char* id, const char* section, const char* key, const char* fallback,
                            const std::vector<std::pair<std::string, std::string>>& choices) {
        const std::string value = iniValue(section, key, fallback);
        int index = -1;
        std::vector<std::string> labels;
        for (size_t i = 0; i < choices.size(); ++i) {
            labels.push_back(choices[i].second);
            if (text::sameText(choices[i].first, value))
                index = static_cast<int>(i);
        }
        // A value the list does not offer shows as itself.
        if (index < 0) {
            labels.push_back(value + " (ERVR.ini)");
            index = static_cast<int>(labels.size()) - 1;
        }
        if (combo(id, &index, labels) && index < static_cast<int>(choices.size()))
            setIni(section, key, choices[index].first);
    };
    // A number shown as `scale` times itself (100: a percentage), set in `step`s.
    const auto amount = [&](const char* id, const char* section, const char* key, float fallback, float lowest,
                            float highest, float scale, float step, const char* shown, int decimals) {
        const std::string raw = iniValue(section, key, "");
        float value = raw.empty() ? fallback : std::strtof(raw.c_str(), nullptr);
        float display = value * scale;
        if (slider(id, &display, lowest, highest, shown)) {
            display = std::round(display / step) * step;
            setIni(section, key, number(display / scale, decimals));
        }
    };

    const float columnWidth = (width - gap) * 0.5f;
    float columnY[2] = {top, top};
    const auto group = [&](int column, const char* title, const std::function<void()>& rows) {
        ImGui::SetCursorScreenPos(ImVec2(base.x + column * (columnWidth + gap), columnY[column]));
        if (beginCard(title, ImVec2(columnWidth, 0), ImGuiChildFlags_AutoResizeY)) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(10), 0));
            cardTitle(title);
            ImGui::Dummy(ImVec2(0, S(8)));
            rows();
            ImGui::PopStyleVar();
        }
        endCard();
        columnY[column] = ImGui::GetItemRectMax().y + gap;
    };
    const float pick = S(170), gauge = S(170), flip = S(42);

    group(0, "Picture", [&] {
        option("View", "Full stereo renders each eye; the big screen shows the flat game in the headset (cheap, "
                       "always works, and what menus and loading screens use).",
               pick, [&] {
                   choice("##stereo", "VR", "StereoMode", "full",
                          {{"full", "Full stereo"}, {"cinema", "Big screen"}, {"off", "Off (flat)"}});
               });
        option("Resolution", "The share of the headset's pixels the game renders. ERVR draws every frame twice: "
                             "lower it if the log warns about stereo pairs per second.",
               gauge, [&] { amount("##res", "VR", "GameResScale", 0.86f, 50, 100, 100, 1, "%.0f%%", 2); });
        option("World scale", "Below 100% the world feels larger, above it smaller (it scales the distance between "
                              "your eyes).",
               gauge, [&] { amount("##world", "VR", "WorldScale", 1.f, 50, 200, 100, 5, "%.0f%%", 2); });
        option("Frame rate follows the headset",
               "Lifts the game's frame cap above the headset's refresh in VR, so the runtime never drops to half rate. "
               "Game speed is untouched.",
               flip, [&] { onOff("##limiter", "VR", "LimiterFollowsHeadset", true); });
        option("Show on the monitor too", "Keeps the game window drawing while the headset shows it.", flip,
               [&] { onOff("##mirror", "VR", "MirrorWindow", true); });
        option("Big screen distance", "Where the big screen hangs, for menus, loading screens and the cinema view.",
               gauge, [&] { amount("##cinemaDistance", "VR", "CinemaDistance", 2.5f, 1, 6, 1, 0.1f, "%.1f m", 2); });
        option("Big screen width", "Its width; with the distance, how much of your view it fills.", gauge,
               [&] { amount("##cinemaWidth", "VR", "CinemaWidth", 4.f, 1, 10, 1, 0.1f, "%.1f m", 2); });
    });
    // The game's own settings, kept in the launcher's: no ERVR.ini needed.
    ImGui::EndDisabled();
    group(0, "Game graphics", [&] {
        const GameGraphics& graphics = scan_.graphics;
        std::string words = "ELDEN RING's own settings, not ERVR's: switched off in its GraphicsConfig.xml now and "
                            "at every START VR. Off here leaves one as the game's menu sets it.";
        if (gameRunning_)
            words = "ELDEN RING is running: it writes its settings back as it closes, so START VR switches these off "
                    "next time.";
        else if (scan_.done && graphics.files.empty())
            words = "ELDEN RING writes its graphics settings on its first start: from the next one, START VR switches "
                    "these off.";
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float wrap = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddText(fonts_.body, S(13.5f), at, col(gameRunning_ ? kAmber : kPaperDim),
                                            words.c_str(), nullptr, wrap);
        ImGui::Dummy(ImVec2(wrap, measure(fonts_.body, S(13.5f), words.c_str(), wrap).y + S(12)));
        firstOption_ = false; // a rule under the words
        for (size_t i = 0; i < kEffectRows.size(); ++i) {
            const std::string& value = graphics.values[i];
            const std::string help = std::string(kEffectRows[i].second) +
                                     (value.empty() ? ""
                                      : text::sameText(value, text::kEffectOff)
                                          ? " Off in the game now."
                                          : " The game has it at " + effectWord(value) + " now.");
            option(kEffectRows[i].first, help.c_str(), flip, [&] {
                bool on = settings_.effectsOff[i];
                if (toggle("##off", &on)) {
                    settings_.effectsOff[i] = on;
                    save();
                    if (on && !gameRunning_)
                        turnOffEffects();
                }
            });
        }
    });
    ImGui::BeginDisabled(!ini_);
    group(0, "HUD", [&] {
        option("HUD", "On a floating panel of its own, sharp and where you can read it; or left in the picture, "
                      "stretched across its edges. Takes effect the next time the game starts.",
               pick, [&] { choice("##hudMode", "HUD", "Mode", "quad", {{"quad", "Floating panel"}, {"off", "In the picture"}}); });
        option("Panel follows", "Your body: stays put while you look around, follows when you turn or walk away. "
                                "Your head: welded to the view.",
               pick, [&] {
                   choice("##hudLock", "HUD", "Lock", "body", {{"body", "Your body"}, {"head", "Your head"}, {"world", "Stays put"}});
               });
        option("Distance", "How far in front of you the panel floats.", gauge,
               [&] { amount("##hudDistance", "HUD", "Distance", 2.f, 0.8f, 4, 1, 0.1f, "%.1f m", 2); });
        option("Width", "The panel's width: smaller takes it in without moving your eyes.", gauge,
               [&] { amount("##hudWidth", "HUD", "Width", 2.4f, 0.8f, 4, 1, 0.1f, "%.1f m", 2); });
        option("Opacity", "Below 100% the world shows through the HUD.", gauge,
               [&] { amount("##hudOpacity", "HUD", "Opacity", 1.f, 20, 100, 100, 5, "%.0f%%", 2); });
    });
    group(0, "Troubleshooting", [&] {
        option("Detailed log", "A heartbeat every 5 seconds with controller values and timings. Turn it on before "
                               "reporting a bug. Takes effect the next time the game starts.",
               flip, [&] {
                   bool debug = text::sameText(iniValue("General", "LogLevel", "info"), "debug");
                   if (toggle("##debug", &debug))
                       setIni("General", "LogLevel", debug ? "debug" : "info");
               });
        option("Numpad test keys", "NumLock on: 7 shows or hides the HUD panel, . re-centres it, 0 logs the state. "
                                   "Each press buzzes the controllers.",
               flip, [&] { onOff("##keys", "VR", "DebugKeys", true); });
        option("ERVR", "Off: the game runs flat with ERVR loaded but idle, as if it were not installed.", flip,
               [&] { onOff("##enabled", "General", "Enabled", true); }, true);
    });

    group(1, "Moving", [&] {
        option("Walk toward", "What forward on the left stick means.", pick, [&] {
            choice("##forward", "Movement", "Forward", "head",
                   {{"head", "Where you look"}, {"hand", "Where the left hand points"}, {"camera", "The stick's view"},
                    {"body", "Where the body faces"}});
        });
        option("Turning", "The right stick turns smoothly, or in steps (snap turns are easier on the stomach).", pick, [&] {
            const bool snap = truthy(iniValue("Movement", "SnapTurn", "false"));
            const int degrees = std::atoi(iniValue("Movement", "SnapTurnDegrees", "30").c_str());
            static constexpr int steps[] = {15, 30, 45, 60, 90};
            int index = 0;
            if (snap) {
                index = 1;
                for (int i = 0; i < 5; ++i)
                    if (std::abs(steps[i] - degrees) < std::abs(steps[index - 1] - degrees))
                        index = i + 1;
            }
            if (combo("##turn", &index,
                      {"Smooth", "Snap 15\xC2\xB0", "Snap 30\xC2\xB0", "Snap 45\xC2\xB0", "Snap 60\xC2\xB0", "Snap 90\xC2\xB0"})) {
                setIni("Movement", "SnapTurn", index ? "true" : "false");
                if (index)
                    setIni("Movement", "SnapTurnDegrees", std::to_string(steps[index - 1]));
            }
        });
        const bool smooth = !truthy(iniValue("Movement", "SnapTurn", "false"));
        ImGui::BeginDisabled(!smooth);
        option("Smooth turn speed", "How fast the right stick turns you, at full tilt.", gauge,
               [&] { amount("##turnSpeed", "Movement", "TurnSpeed", 120, 30, 360, 1, 10, "%.0f\xC2\xB0/s", 0); });
        ImGui::EndDisabled();
        option("Stick deadzone", "How far a stick moves before it counts.", gauge,
               [&] { amount("##deadzone", "Sticks", "Deadzone", 0.15f, 0, 40, 100, 1, "%.0f%%", 2); });
        option("Swap sticks", "Move with the right stick, turn with the left.", flip,
               [&] { onOff("##swap", "Sticks", "Swap", false); });
        option("Stick glitch filter", "For Quest Pro through Virtual Desktop, whose left stick reports jumps. "
                                      "Harmless with other controllers.",
               flip, [&] { onOff("##glitch", "Sticks", "GlitchFilter", true); });
    });
    group(1, "Body", [&] {
        option("First person", "Your head is the camera and the weapons follow your hands. Off: the game's own "
                               "camera, the controllers as a gamepad.",
               flip, [&] { onOff("##fp", "FirstPerson", "Enabled", true); });
        option("Hand-tracked arms", "The character's arms and weapons follow your controllers.", flip,
               [&] { onOff("##hands", "FirstPerson", "HandTracking", true); });
        option("Hide your head", "Keeps hair, hoods and helmets out of your eyes.", flip,
               [&] { onOff("##head", "FirstPerson", "HideHead", true); });
        option("Follow you around the room", "The view's centre slowly follows your head: leaning and ducking still "
                                             "move the view, walking around the room does not.",
               flip, [&] { onOff("##recenter", "FirstPerson", "Recenter", true); });
    });
    group(1, "Combat", [&] {
        option("Physical melee", "Your swing is the attack: the weapon in your hand is the hitbox. Off: swings press "
                                 "the attack buttons and the game plays its animations.",
               flip, [&] { onOff("##melee", "Combat", "Enabled", true); });
        option("Damage follows the swing", "Faster swings with a full windup hit harder; a flick only chips.", flip,
               [&] { onOff("##kinetic", "Combat", "Kinetic", true); });
        option("Damage", "Every swing's damage, the difficulty knob: 50% hits for half.", gauge,
               [&] { amount("##damage", "Combat", "KineticDamageScale", 1.f, 25, 200, 100, 5, "%.0f%%", 2); });
        option("Weapon weight", "Heavy weapons lag behind your hand and resist turning; two hands steer them better.",
               flip, [&] { onOff("##inertia", "Combat", "Inertia", true); });
        option("Two-hand by grabbing", "Put your free hand on the hilt and squeeze its grip to two-hand the weapon.",
               pick, [&] { choice("##twoHand", "Combat", "TwoHand", "grab", {{"grab", "Grab the hilt"}, {"off", "Off"}}); });
        option("Shield blocks", "A shield raised in front of you guards; moving it into a hit deflects it.", flip,
               [&] { onOff("##block", "Combat", "Block", true); });
        option("Spells fly where the hand points", "Spells, ranged skills and bows aim along the casting hand.", flip,
               [&] { onOff("##castAim", "Combat", "CastAim", true); });
        option("Stamina limits swings", "No stamina, no swing (a long buzz says so).", flip,
               [&] { onOff("##stamina", "Combat", "RequireStamina", true); });
    });
    group(1, "Feel", [&] {
        option("Game rumble on the controllers", "The game's own rumble, played on the controllers.", flip,
               [&] { onOff("##rumble", "Haptics", "RumbleToControllers", true); });
        option("Rumble strength", "How strong that rumble is.", gauge,
               [&] { amount("##rumbleScale", "Haptics", "RumbleScale", 1.f, 0, 200, 100, 5, "%.0f%%", 2); });
        option("Swing buzz", "A short buzz when a swing lands.", flip, [&] { onOff("##swingBuzz", "Gestures", "Haptics", true); });
    });
    group(1, "bHaptics", [&] {
        option("Body haptics", "A bHaptics vest, sleeves and visor, through the bHaptics Player (start it and pair "
                               "them there). How strongly they play; 0% is off.",
               gauge, [&] { amount("##bhLevel", "bHaptics", "Level", 100, 0, 100, 1, 5, "%.0f%%", 0); });
        option("Blows taken", "On the side the attacker stands; heals, death and the heart at low HP too.", flip,
               [&] { onOff("##bhDamage", "bHaptics", "Damage", true); });
        option("Your own blows", "Blows landed, blocks and casts in the arm that made them.", flip,
               [&] { onOff("##bhCombat", "bHaptics", "Combat", true); });
        option("Falls and landings", "The wind of a long fall, and the landing from the waist up.", flip,
               [&] { onOff("##bhMovement", "bHaptics", "Movement", true); });
        option("Game rumble on the vest", "The game's own rumble, at most a quarter, on the vest.", flip,
               [&] { onOff("##bhRumble", "bHaptics", "Rumble", true); });
    });
    ImGui::EndDisabled();
    ImGui::SetCursorScreenPos(ImVec2(base.x, std::max(columnY[0], columnY[1])));
    ImGui::Dummy(ImVec2(0, S(2)));
    ImGui::EndChild();
}

void App::controls(ImVec2 origin, ImVec2 size) {
    struct Entry {
        const char* control;
        const char* action;
    };
    struct Group {
        const char* title;
        std::vector<Entry> entries;
    };
    const Group groups[] = {
        {"Moving",
         {{"Left stick", "Walk where you look (Options > Moving)"},
          {"Right stick", "Turn smoothly, or in snaps"},
          {"Your head", "The camera: look around, lean, duck"},
          {"B", "Dodge; hold to sprint"},
          {"A", "Jump; confirm in menus"},
          {"Left stick click", "Crouch"},
          {"Left stick click held + tilt", "D-pad: switch spells, items and weapons"},
          {"Right stick click", "Lock on"}}},
        {"Fighting",
         {{"Swing the right hand", "Attack with the right weapon. The swing is the hitbox: faster hits harder, a full "
                                   "windup hardest"},
          {"Swing the left hand", "Attack with the left weapon"},
          {"Shield raised in front", "Block with a shield in the left hand; moving it into a hit deflects it"},
          {"Left trigger", "Guard"},
          {"Free hand on the hilt, squeeze", "Two-hand the weapon; open the grip to let go"},
          {"Right trigger, two-handing", "The weapon's skill (Ash of War), without its animation"},
          {"Left grip", "Skill"},
          {"A grip held, hand still", "Weapon wheel: that hand's weapons; let go over one to switch to it"},
          {"Cast with a staff, seal or bow", "It flies where the casting hand points; the trigger casts"},
          {"X  /  Y", "Use the item in the quick slot  /  interact (the event action)"}}},
        {"Menus",
         {{"\xE2\x89\xA1 menu button", "Tap: the menu. Hold: the map"},
          {"Left stick, in a menu", "Moves the cursor like a D-pad; keep it tilted to repeat"},
          {"A  /  B", "Confirm  /  back"},
          {"Left stick click held 0.5 s", "Menu navigation by hand on or off, for menus ERVR does not notice opening "
                                          "(grace sites, shops, level up)"}}},
    };
    const float gap = S(18);
    ImGui::SetCursorScreenPos(origin);
    ImGui::BeginChild("##controlsScroll", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(10), 0));
    const ImVec2 base = ImGui::GetCursorScreenPos();
    const float contentWidth = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize - S(6);
    const float columnWidth = (contentWidth - gap) * 0.5f;
    float columnY[2] = {base.y, base.y};
    // Fighting on the right, moving and menus on the left.
    const int columns[] = {0, 1, 0};
    for (int g = 0; g < static_cast<int>(std::size(groups)); ++g) {
        const int column = columns[g];
        ImGui::SetCursorScreenPos(ImVec2(base.x + column * (columnWidth + gap), columnY[column]));
        ImGui::PushID(g);
        if (beginCard("##group", ImVec2(columnWidth, 0), ImGuiChildFlags_AutoResizeY)) {
            cardTitle(groups[g].title);
            ImGui::Dummy(ImVec2(0, S(8)));
            auto* draw = ImGui::GetWindowDrawList();
            const float keyWidth = S(190), keySize = S(14.5f), actionSize = S(14.5f);
            for (const auto& entry : groups[g].entries) {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float width = ImGui::GetContentRegionAvail().x;
                // The control as a key cap in capitals, what it does beside it.
                const std::string key = upper(entry.control);
                const ImVec2 keyExtent = measure(fonts_.label, keySize, key.c_str(), keyWidth - S(16));
                const ImVec2 actionExtent = measure(fonts_.body, actionSize, entry.action, width - keyWidth - S(14));
                const float height = std::max(keyExtent.y + S(10), actionExtent.y) + S(12);
                const ImVec2 capEnd(at.x + std::min(keyExtent.x + S(16), keyWidth), at.y + keyExtent.y + S(10));
                draw->AddRectFilled(at, capEnd, col(kSurfaceHigh), S(3));
                draw->AddRect(at, capEnd, col(kGoldDeep), S(3), 0, S(1));
                draw->AddText(fonts_.label, keySize, ImVec2(at.x + S(8), at.y + S(5)), col(kGoldLift), key.c_str(), nullptr,
                              keyWidth - S(16));
                draw->AddText(fonts_.body, actionSize, ImVec2(at.x + keyWidth + S(14), at.y + S(4)), col(kLogText),
                              entry.action, nullptr, width - keyWidth - S(14));
                ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + height));
                ImGui::Dummy(ImVec2(width, 0));
            }
        }
        endCard();
        ImGui::PopID();
        columnY[column] = ImGui::GetItemRectMax().y + gap;
    }
    ImGui::SetCursorScreenPos(ImVec2(base.x, std::max(columnY[0], columnY[1])));
    if (beginCard("##tips", ImVec2(contentWidth, 0), ImGuiChildFlags_AutoResizeY)) {
        cardTitle("Good to know");
        ImGui::Dummy(ImVec2(0, S(4)));
        ImGui::PushStyleColor(ImGuiCol_Text, vec(kLogText));
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("With physical melee on, the triggers and grips never press the attack buttons: those "
                               "would play attack animations that pull your arms. A staff, seal or bow in the attacking "
                               "hand lets the trigger through, so it casts.");
        ImGui::Dummy(ImVec2(0, S(10)));
        ImGui::TextUnformatted("While a game menu is open, combat stands down: no swings, no injected attacks, and the "
                               "left stick moves the cursor.");
        ImGui::Dummy(ImVec2(0, S(10)));
        ImGui::TextUnformatted("Every button is a key in ERVR.ini ([Buttons], [Gestures], [Combat]). The defaults are "
                               "made for Quest Touch controllers; Options has the settings players change most.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, S(4)));
    }
    endCard();
    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

void App::about(ImVec2 origin, ImVec2 size) {
    ImGui::SetCursorScreenPos(origin);
    ImGui::BeginChild("##aboutScroll", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    const ImVec2 base = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize - S(6);
    const float gap = S(18);
    const float rightWidth = std::clamp(width * 0.42f, S(360), S(480));
    const float leftWidth = width - gap - rightWidth;
    ImGui::SetCursorScreenPos(base);
    if (beginCard("##about", ImVec2(leftWidth, 0), ImGuiChildFlags_AutoResizeY)) {
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        emblem(draw, ImVec2(at.x + S(34), at.y + S(36)), S(34));
        wordmark(draw, ImVec2(at.x + S(88), at.y + S(2)), S(46));
        const float kicker = S(13.5f);
        spacedText(draw, fonts_.kicker, kicker, ImVec2(at.x + S(90), at.y + S(54)), col(kPaperDim), "CREATED BY", 0.14f);
        spacedText(draw, fonts_.kicker, kicker,
                   ImVec2(at.x + S(90) + spacedWidth(fonts_.kicker, kicker, "CREATED BY ", 0.14f), at.y + S(54)),
                   col(kPaper), upper(kAuthor).c_str(), 0.14f);
        ImGui::Dummy(ImVec2(0, S(80)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(10), S(10)));
        ImGui::PushStyleColor(ImGuiCol_Text, vec(kLogText));
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("ERVR puts you inside ELDEN RING: an image for each eye, your head as the camera, the "
                               "weapons in your hands, and melee where your swing is the attack.");
        ImGui::TextUnformatted("It runs inside the game as a ReShade add-on with an OpenXR session of its own: "
                               "dinput8.dll loads ERVR\\ERVR.dll, and dxgi.dll is ReShade (bundled). This launcher "
                               "copies those files, starts the game and reads ERVR's log; it changes nothing else.");
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, vec(kAmber));
        ImGui::TextUnformatted("Offline only, with EAC off. Never run it with Easy Anti-Cheat or FromSoftware's online "
                               "play: that risks a ban. Seamless Co-op is the multiplayer that works.");
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, vec(kLogText));
        ImGui::TextUnformatted("No telemetry, no analytics, no auto-updater, no network code of their own: neither ERVR "
                               "nor this launcher goes online. Body haptics use bHaptics' own library, which talks to the "
                               "bHaptics Player on this PC. New versions are on GitHub Releases and Nexus Mods.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, S(2)));
        ImGui::PushStyleColor(ImGuiCol_Text, vec(kFaint));
        ImGui::PushTextWrapPos(0);
        ImGui::Text("A free fan project under GPL-3.0-or-later, not affiliated with FromSoftware, Bandai Namco, Meta, "
                    "Valve or the ReShade project. ERVR %s. Built with ReShade (BSD 3-clause), the OpenXR loader "
                    "(Apache 2.0), MinHook (BSD), Dear ImGui (MIT) and the Cinzel and Barlow fonts (SIL OFL).",
                    kVersion);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        if (link("Releases"))
            openUrl(kReleasesUrl);
        ImGui::SameLine(0, S(18));
        if (link("Source"))
            openUrl(kRepoUrl);
        ImGui::SameLine(0, S(18));
        if (link("Third-party notices"))
            openUrl("https://github.com/IlyaMez/EldenringVR/blob/main/THIRD_PARTY.md");
        ImGui::PopStyleVar();
        ImGui::Dummy(ImVec2(0, S(2)));
    }
    endCard();
    ImGui::SetCursorScreenPos(ImVec2(base.x, ImGui::GetItemRectMax().y + gap));
    if (beginCard("##pc", ImVec2(leftWidth, 0), ImGuiChildFlags_AutoResizeY)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(8)));
        cardTitle("On this PC");
        const float full = ImGui::GetContentRegionAvail().x;
        const bool game = !scan_.gameDir.empty();
        if (button("Add desktop and Start menu shortcuts", full, Ink::plain, kShortcut)) {
            std::string error;
            shortcutMessage_ = createShortcuts(error) ? "Shortcuts added: look for ELDEN RING VR (ERVR)." : error;
            settings_.shortcutsAsked = true;
            save();
        }
        ImGui::BeginDisabled(!game);
        if (button("Open the game folder", full, Ink::plain, kFolder))
            openPath(scan_.gameDir);
        if (button("Open ERVR's folder (its settings and logs)", full, Ink::plain, kFolder))
            openPath((std::filesystem::path(scan_.gameDir) / L"ERVR").wstring());
        ImGui::BeginDisabled(session_.running() || gameRunning_ || scan_.install.state == InstallState::missing);
        if (button("Play flat: park ERVR, put the flat mods back", full, Ink::plain, kSwitch))
            ask_ = Ask::flat;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            tip("Renames ERVR's dinput8.dll to dinput8.dll.ervr-off and restores the parked flat render mods. START VR "
                "switches back.");
        if (button("Remove ERVR from the game", full, Ink::danger, kDelete))
            ask_ = Ask::uninstall;
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
        if (!shortcutMessage_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, vec(kPaperDim));
            ImGui::TextWrapped("%s", shortcutMessage_.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, S(2)));
    }
    endCard();
    // Right: the tip jar and the Discord, as the README has them.
    ImGui::SetCursorScreenPos(ImVec2(base.x + leftWidth + gap, base.y));
    ImGui::BeginGroup();
    supportCards(rightWidth);
    ImGui::EndGroup();
    ImGui::EndChild();
}

void App::supportCards(float width) {
    struct Card {
        const char* id;
        Rgb accent;
        const char* kicker;
        const char* lines[2];
        const char* words;
        const char* loud;
        const char* address;
        const char* url;
        bool tipJar; // keylined in gold, its address a button with a heart
    };
    const Card cards[] = {
        {"##kofiCard", kGoldLift, "ERVR is free. Every build, always.", {"Buy me", "a coffee"},
         "A tip keeps the updates coming.", "Nothing is or will be paywalled.", "ko-fi.com/ilyamezerowsky", kKofiUrl, true},
        {"##discordCard", kPaper, "Where the players are", {"Join the", "Discord"},
         "Release news, help in #ervr-help, and clips.", "Come say hi.", "discord.gg/x27ZXDpdNt", kDiscordUrl, false},
    };
    for (const auto& card : cards) {
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float pad = S(22), kickerSize = S(13.5f), headSize = S(36), addressSize = S(16);
        const float captionWidth = width - pad * 2;
        // Measured first, drawn after: the card is as tall as what it holds.
        const float captionHeight = captionBox(nullptr, at, captionWidth, card.words, Caption::narrator, card.loud);
        const float height = pad + kickerSize + S(14) + headSize * 2.2f + S(14) + captionHeight + S(18) + addressSize +
                             S(16) + pad;
        const ImVec2 end(at.x + width, at.y + height);
        const bool clicked = ImGui::InvisibleButton(card.id, ImVec2(width, height));
        handCursor();
        const bool hovered = ImGui::IsItemHovered();
        draw->AddRectFilled(at, end, col(hovered ? kSurfaceHigh : kSurface));
        draw->PushClipRect(at, end, true);
        glow(draw, end, width * 0.8f, kGold, card.tipJar ? 0.12f : 0.05f);
        draw->PopClipRect();
        draw->AddRect(at, end, card.tipJar ? col(kGold) : col(hovered ? kGoldDeep : kLine), 0, 0, S(card.tipJar ? 1.5f : 1));
        corners(draw, at, end, S(12), col(kGold), S(2));
        float y = at.y + pad;
        diamond(draw, ImVec2(at.x + pad + S(4), y + kickerSize * 0.52f), S(4), col(kGold));
        spacedText(draw, fonts_.kicker, kickerSize, ImVec2(at.x + pad + S(16), y), col(kPaperDim),
                   upper(card.kicker).c_str(), 0.12f);
        y += kickerSize + S(14);
        engraved(draw, fonts_.display, headSize, ImVec2(at.x + pad, y), col(kPaper), upper(card.lines[0]).c_str(), 0.04f);
        engraved(draw, fonts_.display, headSize, ImVec2(at.x + pad, y + headSize * 1.08f), col(card.accent),
                 upper(card.lines[1]).c_str(), 0.04f);
        y += headSize * 2.2f + S(14);
        captionBox(draw, ImVec2(at.x + pad, y), captionWidth, card.words, Caption::narrator, card.loud);
        y += captionHeight + S(18);
        // Its address as a button, at the right.
        const char* heart = card.tipJar && fonts_.icons ? kHeart : nullptr;
        const float heartWidth = heart ? measure(fonts_.label, addressSize, heart).x + S(8) : 0;
        const float addressWidth = measure(fonts_.label, addressSize, card.address).x + heartWidth + S(32);
        const ImVec2 pill(end.x - pad - addressWidth, y), pillEnd(end.x - pad, y + addressSize + S(16));
        const ImU32 lettering = col(card.tipJar ? kGoldLift : kPaper);
        draw->AddRectFilled(pill, pillEnd, col(hovered ? kSurfaceHover : kSurfaceHigh), S(3));
        draw->AddRect(pill, pillEnd, card.tipJar ? col(kGold) : col(hovered ? kGoldDeep : kLine), S(3), 0,
                      S(card.tipJar ? 1.5f : 1));
        if (heart)
            draw->AddText(fonts_.label, addressSize, ImVec2(pill.x + S(16), pill.y + S(7)), lettering, heart);
        draw->AddText(fonts_.label, addressSize, ImVec2(pill.x + S(16) + heartWidth, pill.y + S(8)), lettering,
                      card.address);
        if (clicked)
            openUrl(card.url);
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + height + S(18)));
    }
    ImGui::Dummy(ImVec2(width, 0));
}

void App::modals() {
    static constexpr const char* names[] = {"",           "Switch to VR?",      "The game is running", "Play flat?",
                                            "Remove ERVR?", "Restore defaults?", "Administrator rights"};
    if (ask_ != Ask::none) {
        ImGui::OpenPopup(names[static_cast<int>(ask_)]);
        ask_ = Ask::none;
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    // 1: yes, 2: the second choice (when there is one), 0: no; -1 while open or closed.
    const auto dialog = [&](const char* name, const std::string& message, const char* yes, const char* other,
                            const char* no, bool danger = false) -> int {
        int answer = -1;
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(S(520), 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(26), S(24)));
        ImGui::PushStyleColor(ImGuiCol_Border, vec(kGoldDeep));
        if (ImGui::BeginPopupModal(name, nullptr,
                                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
            auto* draw = ImGui::GetWindowDrawList();
            corners(draw, ImGui::GetWindowPos(), add(ImGui::GetWindowPos(), ImGui::GetWindowSize()), S(12), col(kGold), S(2));
            engraved(draw, fonts_.display, S(26), ImGui::GetCursorScreenPos(), col(kPaper), upper(name).c_str(), 0.04f);
            ImGui::Dummy(ImVec2(0, S(34)));
            ImGui::PushStyleColor(ImGuiCol_Text, vec(kPaperDim));
            ImGui::TextWrapped("%s", message.c_str());
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, S(10)));
            const int count = other ? 3 : 2;
            const float buttonW = (ImGui::GetContentRegionAvail().x - S(12) * (count - 1)) / count;
            if (button(yes, buttonW, danger ? Ink::danger : Ink::primary))
                answer = 1;
            if (other) {
                ImGui::SameLine(0, S(12));
                if (button(other, buttonW))
                    answer = 2;
            }
            ImGui::SameLine(0, S(12));
            if (button(no, buttonW) || ImGui::IsKeyPressed(ImGuiKey_Escape))
                answer = 0;
            if (answer >= 0)
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        return answer;
    };
    if (ImGui::IsPopupOpen(names[1])) {
        std::vector<std::string> live;
        for (const auto& mod : scan_.install.flat)
            if (!mod.parked)
                live.push_back(mod.label);
        std::string message = "Before VR starts, the launcher renames these to *.ervr-off (nothing is deleted; About > "
                              "Play flat puts them back):\n\n";
        for (const auto& item : live)
            message += "  \xC2\xB7 " + item + "\n";
        if (scan_.install.state == InstallState::parked)
            message += "  \xC2\xB7 ERVR's dinput8.dll comes back from dinput8.dll.ervr-off\n";
        if (dialog(names[1], message, "Switch and start", nullptr, "Cancel") == 1)
            begin(true, false);
    }
    if (ImGui::IsPopupOpen(names[2])) {
        const int answer = dialog(names[2], "ELDEN RING is already running. The launcher can follow it as it is (its log "
                                            "shows here; if it started without ERVR, VR will not come up), or close it "
                                            "so that START VR starts it again.",
                                  "Follow it", "Close it", "Cancel");
        if (answer == 1)
            begin(false, true);
        else if (answer == 2)
            note(LineKind::normal, askGameToClose(scan_.gameDir)
                                       ? "Asked ELDEN RING to close. Press START VR once it has."
                                       : "ELDEN RING has no window to close; close it yourself, then press START VR.");
    }
    if (ImGui::IsPopupOpen(names[3]) &&
        dialog(names[3], "ERVR's dinput8.dll is renamed to dinput8.dll.ervr-off, so the game starts without it, and the "
                         "flat render mods parked for VR come back. START VR switches back.",
               "Play flat", nullptr, "Cancel") == 1)
        apply(switchToFlat(scan_), "Switching to flat play");
    if (ImGui::IsPopupOpen(names[4]) &&
        dialog(names[4], "Removes dinput8.dll, ERVR\\ERVR.dll and the bundled ReShade (dxgi.dll; a dxgi.dll ERVR had "
                         "replaced comes back). ERVR.ini, the logs and ReShade.ini stay, so a reinstall keeps your "
                         "settings.",
               "Remove", nullptr, "Cancel", true) == 1)
        apply(uninstallErvr(scan_), "Removing ERVR");
    if (ImGui::IsPopupOpen(names[5]) &&
        dialog(names[5], "The game folder's ERVR.ini is replaced by this launcher's (ERVR " + scan_.payloadVersion +
                             "'s defaults). Yours is kept beside it as ERVR.ini.<date>.bak.",
               "Restore defaults", nullptr, "Cancel") == 1)
        apply(restoreDefaultIni(scan_), "Restoring ERVR.ini");
    if (ImGui::IsPopupOpen(names[6])) {
        const int answer = dialog(names[6], (administratorError_.empty() ? std::string() : administratorError_ + "\n\n") +
                                                "Windows kept the launcher from changing files in the game folder. It can "
                                                "start again as administrator; Windows asks you to allow it.",
                                  "Restart as administrator", nullptr, "Not now");
        if (answer == 1) {
            if (restartAsAdministrator(administratorError_))
                quit_ = true;
            else if (!administratorError_.empty())
                ask_ = Ask::administrator;
        }
    }
}

} // namespace launcher
