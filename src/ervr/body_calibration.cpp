#include "body_calibration.h"
#include "body_calibration_draw.h"
#include "body_calibration_logic.h"
#include "config.h"
#include "debug_keys.h"
#include "game_er.h"
#include "ini_file.h"
#include "launcher_text.hpp"
#include "log.h"
#include "overlay_canvas.h"
#include "overlay_layers.h"
#include "weapon_wheel_logic.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ervr {
namespace {

constexpr double kPromptAfter = 1.5;           // s of play before the panel asks
constexpr double kResultShows = 3.5;           // s the result stays up

std::atomic<bool> g_request{false};
// A measurement just taken, until the hot reload shows ERVR.ini holding it (or 5 s pass): the body uses it at once.
std::atomic<int> g_pendingEye{0}, g_pendingArm{0};
double g_pendingSince = 0.0;

BodyCalibration g_cal;
bool g_running = false;      // asked for: the panel shows whenever there is play
bool g_shown = false;        // it showed last update (a pause hides it)
bool g_requested = false;    // this one came from BodyCalibrationRequest, not from [Body] lacking one
double g_playSince = -1.0, g_last = 0.0, g_doneAt = 0.0, g_startedAt = 0.0;
Pose g_panel;
const char* g_lastWhy = nullptr;
CalibHint g_lastHint = CalibHint::none;
CalibPhase g_lastPhase = CalibPhase::idle;
// Each control held back until let go: [0] left trigger, [1] right trigger, [2] left grip, [3] right grip, [4] A, [5] B.
bool g_latch[6] = {};
bool g_bWasDown = false;

void Measurements(const Config& c, int& eye, int& arm) {
    eye = c.bodyEyeHeightMm; arm = c.bodyArmLengthMm;
    const int pe = g_pendingEye.load(std::memory_order_relaxed), pa = g_pendingArm.load(std::memory_order_relaxed);
    if (pe > 0 && pa > 0) { eye = pe; arm = pa; }
}

// Why this is no moment for the panel (null: it is).
const char* NotPlaying(const Config& c, const VRTracking& tr, bool menu) {
    if (!c.fpEnabled || !c.handTracking) return "first person or the hand-tracked arms are off";
    if (!tr.head.valid) return "the headset is not tracked";
    if (!GameERPlayerActive()) return "no character in the world";
    if (GameERTransitionActive()) return "a load or a warp";
    const MoveBasis mb = GameERGetMoveBasis();
    if (!mb.active) return "first person is not running";
    if (mb.riding) return "riding Torrent";
    if (menu) return "a menu";
    if (GameERForeignCamActive()) return "a cutscene or the map";
    if (GameERPlayerDead()) return "the character died";
    if (GetTickCount64() - GameERLastStashTick() > 500) return "the headset is not showing the game in stereo";
    return nullptr;
}

// Where ERVR puts the Tarnished's wrist for a controller (game_er.cpp's arm target: the grip pose HandBackOffset
// back along the controller's forward, as HandRotOffset turns it), back in tracking space.
Vec3 Wrist(const Config& c, const Pose& grip) {
    float rotX = c.handRotOffX;
    DebugKeysHandRotX(&rotX);
    const Quat calib = Quat::FromEulerYXZ(Deg2Rad(c.handRotOffY), Deg2Rad(rotX), Deg2Rad(c.handRotOffZ));
    const Quat q{-grip.rot.x, -grip.rot.y, grip.rot.z, grip.rot.w};   // game frame: Z mirrored
    const Vec3 p{grip.pos.x, grip.pos.y, -grip.pos.z};
    const Vec3 w = p - (q * calib).Normalized().Rotate({0.f, 0.f, 1.f}) * c.handBackOffset;
    return {w.x, w.y, -w.z};
}

// ERVR.ini [Body]: what the calibration writes. False when Windows refused the file.
bool SaveBody(const std::vector<std::pair<const char*, std::string>>& keys, std::string& said) {
    const std::wstring& path = Cfg().iniPath;
    std::string text = IniReadAll(path);
    if (text.empty()) return false;
    for (const auto& k : keys) {
        text = launcher::text::iniSet(text, "Body", k.first, k.second);
        said += (said.empty() ? "" : ", ") + std::string(k.first) + "=" + k.second;
    }
    return IniWriteAll(path, text, L".body");
}

// ---------------------------------------------------------------- drawing (body_calibration_draw.h)
CalibPanelState g_panelKey;
Canvas g_base, g_cv;
int g_baseDone = -1;                       // which base g_base holds: 0 asking, 1 measured, -1 none

void DrawPanel(const CalibPanelState& s) {
    const bool done = s.phase == CalibPhase::done;
    if (g_baseDone != (done ? 1 : 0)) { CalibDrawBase(g_base, done); g_baseDone = done ? 1 : 0; }
    CalibDrawPanel(g_base, g_cv, s);
    std::vector<uint32_t> img;
    g_cv.EncodeSrgb(img, 1.f);
    OverlayPublishImage(OverlayCalibPanel, kCalibPanelPxW, kCalibPanelPxH, img);
}

struct RingKey {
    bool lit = false;
    int progress = -1;
    bool operator==(const RingKey& o) const { return lit == o.lit && progress == o.progress; }
};
RingKey g_ringKey[2];

void DrawRing(int h, const RingKey& key) {
    Canvas cv;
    CalibDrawRing(cv, key.lit, key.progress);
    std::vector<uint32_t> img;
    cv.EncodeSrgb(img, 1.f);
    OverlayPublishImage(OverlayCalibRingL + h, kCalibRingPx, img);
}

void Hide() {
    OverlayPublishPose(OverlayCalibPanel, false, Pose{}, 0.f);
    OverlayPublishPose(OverlayCalibRingL, false, Pose{}, 0.f);
    OverlayPublishPose(OverlayCalibRingR, false, Pose{}, 0.f);
}

void Show(const VRTracking& tr, const CalibPanelState& panel, bool ringsLit[2], int ringProgress) {
    if (!(panel == g_panelKey)) { g_panelKey = panel; DrawPanel(panel); }
    OverlayPublishPose(OverlayCalibPanel, true, g_panel, kCalibPanelWidth, kCalibPanelHeight);
    for (int h = 0; h < 2; ++h) {
        const Pose& hand = tr.hand[h];
        const bool on = hand.valid && tr.head.valid && (tr.head.pos - hand.pos).Length() > 0.05f;
        if (on) {
            const RingKey key{ringsLit[h], ringProgress};
            if (!(key == g_ringKey[h])) { g_ringKey[h] = key; DrawRing(h, key); }
            Pose p;
            const WheelFrame f = WheelFrameFacing(hand.pos, tr.head.pos, tr.head.rot.Up());
            p.pos = hand.pos; p.rot = WheelOrientation(f); p.valid = true;
            OverlayPublishPose(OverlayCalibRingL + h, true, p, kCalibRingM);
        } else {
            OverlayPublishPose(OverlayCalibRingL + h, false, Pose{}, 0.f);
        }
    }
}

void Begin(const VRTracking& tr, double now, const char* why) {
    g_cal.Start();
    g_running = true;
    g_startedAt = now;
    g_panel = CalibPanelPose(tr.head.pos, tr.head.rot);
    g_lastHint = CalibHint::none; g_lastPhase = CalibPhase::waiting;
    LOGI("[calib] body calibration: the T-pose panel is up (%s) - stand tall, arms straight out to the sides, both triggers; B skips", why);
}

void End() {
    g_cal.Stop();
    g_running = false;
    g_requested = false;
    g_playSince = -1.0;
    Hide();
}

} // namespace

void BodyCalibrationRequest() { g_request.store(true); }

float BodyEyeHeight() {
    int eye = 0, arm = 0;
    Measurements(Cfg(), eye, arm);
    return CalibValid(eye, arm) ? eye / 1000.f : 0.f;
}
float BodyArmLength() {
    int eye = 0, arm = 0;
    Measurements(Cfg(), eye, arm);
    return CalibValid(eye, arm) ? arm / 1000.f : 0.f;
}

CalibPadOut BodyCalibrationUpdate(const VRTracking& tr, const VRInputState& in, double now, bool menu) {
    const Config& c = Cfg();
    CalibPadOut out;
    CanvasInit();
    const float dt = g_last > 0.0 ? Clamp((float)(now - g_last), 0.f, 0.1f) : 0.f;
    g_last = now;

    // A measurement just taken stays in use until the reload has brought it (or ERVR.ini refused it, 5 s on).
    {
        const int pe = g_pendingEye.load(), pa = g_pendingArm.load();
        if (pe > 0 && ((c.bodyEyeHeightMm == pe && c.bodyArmLengthMm == pa) || now - g_pendingSince > 5.0)) {
            g_pendingEye.store(0); g_pendingArm.store(0);
        }
    }
    int eyeMm = 0, armMm = 0;
    Measurements(c, eyeMm, armMm);
    const bool calibrated = CalibValid(eyeMm, armMm);
    if (g_request.exchange(false)) {
        if (!g_running) { g_requested = true; g_playSince = -1.0; LOGI("[calib] body calibration asked for: the panel comes up at the next moment of play"); }
    }
    const bool wanted = g_requested || (!calibrated && c.bodyCalibrationPrompt);
    const char* why = NotPlaying(c, tr, menu);
    if (why != g_lastWhy) {
        if (wanted || g_running) LOGD("[calib] %s", why ? why : "play: the panel may show");
        g_lastWhy = why;
    }

    bool showing = false;
    if (g_running) {
        if (g_cal.Phase() == CalibPhase::done) {
            showing = now - g_doneAt < kResultShows;
            if (!showing) End();
        } else if (!wanted) {
            // The launcher's Redo brought a calibration back, or the ini turned the prompt off under it.
            LOGI("[calib] body calibration closed: ERVR.ini [Body] %s", calibrated ? "holds one again" : "no longer asks for one");
            End();
        } else if (why) {
            if (g_shown) LOGI("[calib] body calibration held: %s (it comes back with play)", why);
        } else {
            if (!g_shown) g_panel = CalibPanelPose(tr.head.pos, tr.head.rot);   // back from a pause: where you face now
            showing = true;
        }
    } else if (wanted && !why) {
        if (g_playSince < 0.0) g_playSince = now;
        if (now - g_playSince >= kPromptAfter) {
            Begin(tr, now, g_requested ? "asked for" : "no calibration in ERVR.ini [Body] yet");
            showing = true;
        }
    } else {
        g_playSince = -1.0;
    }

    // B skips; the press is the panel's either way.
    const bool bDown = (in.buttons & VRB_B) != 0;
    if (showing && g_cal.Phase() != CalibPhase::done && bDown && !g_bWasDown) {
        std::string said;
        const bool saved = SaveBody({{"CalibrationPrompt", "false"}}, said);
        LOGI("[calib] body calibration skipped (B) after %.1f s, last hint: %s; %s", now - g_startedAt, CalibHintName(g_cal.Hint()),
             saved ? "ERVR.ini [Body] CalibrationPrompt=false: the launcher's Options > Body calibration asks again"
                   : "ERVR.ini could not be written: it asks again next session");
        End();
        showing = false;
        g_latch[5] = true;   // this B is no dodge
    }
    g_bWasDown = bDown;

    if (showing && g_cal.Phase() != CalibPhase::done) {
        CalibSample s;
        s.headTracked = tr.head.valid;
        s.head = tr.head.pos; s.headRot = tr.head.rot;
        for (int h = 0; h < 2; ++h) {
            s.handTracked[h] = tr.hand[h].valid && in.controllerConnected[h];
            s.wrist[h] = s.handTracked[h] ? Wrist(c, tr.hand[h]) : Vec3{};
            s.trigger[h] = in.indexTrigger[h];
        }
        s.seconds = dt;
        if (g_cal.Update(s)) {
            const CalibResult& r = g_cal.Result();
            const int eye = (int)std::lround(r.eyeHeight * 1000.f), arm = (int)std::lround(r.armLength * 1000.f);
            g_doneAt = now;
            if (CalibValid(eye, arm)) {
                std::string said;
                const bool saved = SaveBody({{"EyeHeightMm", std::to_string(eye)}, {"ArmLengthMm", std::to_string(arm)},
                                             {"CalibrationPrompt", "true"}}, said);
                g_pendingEye.store(eye); g_pendingArm.store(arm);
                g_pendingSince = saved ? now : 1e18;   // unsaved: in use for the rest of the session
                LOGI("[calib] body calibrated in %.1f s: eyes %.3f m up (height %.2f m), arms %.3f m (left %.3f, right %.3f); %s %s",
                     now - g_startedAt, r.eyeHeight, CalibHeight(r.eyeHeight), r.armLength, r.reach[0], r.reach[1],
                     saved ? "ERVR.ini [Body]:" : "ERVR.ini could not be written - kept for this session only:", said.c_str());
                for (int h = 0; h < 2; ++h) { out.hapticAmp[h] = 0.8f; out.hapticSec[h] = 0.15f; }
            } else {
                LOGW("[calib] body calibration measured eyes %.3f m up and arms %.3f m, outside what ERVR keeps: not used, asking again",
                     r.eyeHeight, r.armLength);
                g_cal.Start();
            }
        }
        if (g_cal.Hint() != g_lastHint || g_cal.Phase() != g_lastPhase) {
            LOGD("[calib] %s, hint: %s (arms in place: left %d, right %d)",
                 g_cal.Phase() == CalibPhase::holding ? "holding" : g_cal.Phase() == CalibPhase::done ? "done" : "waiting",
                 CalibHintName(g_cal.Hint()), (int)g_cal.ArmReady(0), (int)g_cal.ArmReady(1));
            g_lastHint = g_cal.Hint(); g_lastPhase = g_cal.Phase();
        }
    }

    if (showing) {
        CalibPanelState key;
        key.phase = g_cal.Phase();
        key.hint = g_cal.Hint();
        key.ready = g_cal.ArmReady(0) && g_cal.ArmReady(1);
        key.progress = key.phase == CalibPhase::done ? kCalibSteps : (int)(g_cal.Progress() * kCalibSteps);
        if (key.phase == CalibPhase::done) {
            key.eyeMm = (int)std::lround(g_cal.Result().eyeHeight * 1000.f);
            key.armMm = (int)std::lround(g_cal.Result().armLength * 1000.f);
            key.hint = CalibHint::none; key.ready = true;
        }
        bool lit[2] = {key.phase == CalibPhase::done || g_cal.ArmReady(0), key.phase == CalibPhase::done || g_cal.ArmReady(1)};
        Show(tr, key, lit, key.phase == CalibPhase::done ? 0 : key.progress);
    } else if (g_shown) {
        Hide();
    }
    g_shown = showing;

    // While it asks, the triggers, grips, A and B are its own; each stays held back until let go.
    const bool asking = showing && g_cal.Phase() != CalibPhase::done;
    const bool down[6] = {in.indexTrigger[0] > 0.1f, in.indexTrigger[1] > 0.1f, in.handTrigger[0] > 0.1f, in.handTrigger[1] > 0.1f,
                          (in.buttons & VRB_A) != 0, bDown};
    for (int i = 0; i < 6; ++i) g_latch[i] = down[i] && (asking || g_latch[i]);
    for (int h = 0; h < 2; ++h) { out.trigger[h] = g_latch[h]; out.grip[h] = g_latch[2 + h]; }
    if (g_latch[4]) out.buttons |= VRB_A;
    if (g_latch[5]) out.buttons |= VRB_B;
    out.showing = asking;
    return out;
}

} // namespace ervr
