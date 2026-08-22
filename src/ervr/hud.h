#pragma once
// Lifts ELDEN RING's UI out of the rendered frame and onto its own composition layer.
//
// WHY
// ---
// In full stereo each eye is a CROP of one very wide game frame (the camera is widened to
// ~110 deg vertical so a single image can contain a whole eye's field of view). The HUD is
// laid out in screen space, so the engine paints it along the edges of that widened frame -
// i.e. 50+ degrees off-axis, half of it outside the eye crop, the rest smeared across the
// periphery at the wrong depth. There is no camera setting that fixes it: the HUD is not in
// the world, it is on the film.
//
// HOW
// ---
// So it is taken off the film. ERVR is already a ReShade add-on, which means it sees the
// game's own D3D12 command stream. The UI draws are recognised (see "FINDING THE UI"
// below), and the instant the first one is issued the render target under them is swapped
// for a texture of ERVR's own, cleared transparent:
//
//   * the game's frame comes out with NO UI in it - the eye crops are pure world, and
//     nothing about the stereo path changes;
//   * ERVR's texture comes out holding the UI alone, over transparency;
//   * that texture is submitted as a second quad layer, world- or head-anchored at a
//     distance and size the player picks, blended over the projection layer by the
//     compositor at full headset resolution.
//
// The redirect is one OMSetRenderTargets per UI draw batch. Nothing is skipped, no shader
// is patched, no game code is hooked - if the detection is wrong the worst case is that a
// post-processing pass lands on the quad instead of the frame, which is instantly visible
// and instantly undone (Numpad 7).
//
// FINDING THE UI
// --------------
// The UI is "the draws that land on the final image after the frame has been resolved onto
// it". ERVR does not need to know which shaders those are: it counts draws per render
// target, and everything after the first `FirstDraw` draws into the frame's final target is
// UI. On ELDEN RING that first draw is the tonemap/output pass and the rest is Scaleform
// plus the sprite HUD, so FirstDraw=1 is the shipping default.
//
// [HUD] Probe=true dumps the frame's render-target/draw census to the log once a second -
// which target got how many draws, in what order - which is how a different build, a
// ReShade effect chain in the way, or a future patch gets re-tuned. Numpad 8 steps
// FirstDraw live, in the headset.
#include <cstdint>
#include <dxgiformat.h>

struct ID3D12Resource;

namespace ervr {

// Registered alongside ERVR's other ReShade events (reshade_addon.cpp).
void HudRegisterEvents();
void HudUnregisterEvents();
// Render thread, from the present handler: closes the pass's capture and re-arms for the
// next one. Must be called on EVERY present, whether or not the HUD is enabled.
// `reshadeSwapchain` is the reshade::api::swapchain* the present carried - ERVR registers
// as an add-on after the game is already running, so init_swapchain never fires for the
// swapchain that already exists and this is the only place the back buffers can be learned.
void HudOnPresent(void* reshadeSwapchain);
void HudOnDeviceLost();

// [HUD] MirrorPanel: composite the reconstructed panel back onto the game's own frame, so
// the monitor - and an unattended screenshot - shows exactly what the headset will get,
// coverage and colour and all. Without it the panel can only be judged by wearing the
// headset, which no automated run can do.
struct Dx12Frame;
void HudMirrorPanel(const Dx12Frame& f);

// True when the ini asks for the HUD layer at all. Cheap; call freely. Note this is the
// player's INTENT - the redirect additionally requires somewhere to put the UI, see below.
bool HudEnabled();

// Render thread, from the stereo present path, whenever there is a live VR session with
// graphics bound - i.e. whenever there is actually a composition layer to hang the UI on.
// The redirect follows this: pulling the UI out of the frame when nothing is going to
// submit it just loses the HUD, which is exactly what an unattended flat run does
// (measured 2026-08-21: 170 s of play with the headset not streaming and no HUD anywhere).
void HudNoteSubmitLive();

// This pass's UI image, or valid=false when the pass drew no UI (loading, a cutscene with
// the HUD off, or detection that has not caught yet). Owned by the HUD module and stable
// until the next present; the blitter is told it is in RENDER_TARGET state.
struct HudFrame {
    bool            valid = false;
    ID3D12Resource* tex = nullptr;            // the UI over a transparent-black clear
    // The same UI over an opaque-white clear, or null when it could not be captured twice.
    // The pair is what makes coverage exact - see BlitParams::Alpha::Dual.
    ID3D12Resource* texOverWhite = nullptr;
    DXGI_FORMAT     format = DXGI_FORMAT_UNKNOWN;
    uint32_t        width = 0, height = 0;
};
bool HudTakeFrame(HudFrame& out);

// Numpad 7 / 8 (debug_keys.cpp). Overrides that outlive an ini hot-reload for the session.
void HudSetEnabledOverride(int on);   // -1 = follow the ini, 0 = off, 1 = on
int  HudGetEnabledOverride();
void HudStepFirstDraw(int delta);     // clamped to [0, 16]; logs the value to bake in
int  HudFirstDraw();                  // the value in force right now
// Drop the panel where the player is looking now (Numpad . ). Matters most with Lock=world,
// where it is the only thing that ever moves the quad.
void HudRequestRecenter();
bool HudTakeRecenter();               // consumes the one-shot; render thread

struct HudStats {
    long captures = 0;       // passes that produced a UI image, since the last read
    long redirects = 0;      // UI draws redirected, since the last read
    long misses = 0;         // passes that found no UI draws at all
    int  lastDrawsIntoTarget = 0;
    int  firstDraw = 0;
    bool targetFound = false;
};
HudStats HudGetStats();
}
