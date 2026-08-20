// OpenXR backend - ERVR's own XrSession, bound to the game's D3D12 device and queue.
//
// This replaces the old parasitic LibOVR/OpenVR backends that captured R.E.A.L.'s session.
// Two planes live here:
//   * pose/input  - called from the worker thread at PollHz, feeding GetLatestVRState().
//   * frame loop  - called from the render thread (inside ReShade's present event) by
//                   stereo.cpp only: WaitBeginFrame / LocateViews / Acquire / EndFrame.
// OpenXR requires external synchronisation on the session, so every session-touching call
// takes g_mtx. xrWaitFrame is deliberately made outside the lock: it blocks until the
// runtime's next frame cadence and would otherwise stall the worker's input polling.
#include "vr_backend.h"
#include "config.h"
#include "log.h"

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <windows.h>
#include <d3d12.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

namespace ervr {
namespace {

std::recursive_mutex g_mtx;

const char* XrStr(XrResult r) {
    switch (r) {
    case XR_SUCCESS: return "XR_SUCCESS";
    case XR_SESSION_LOSS_PENDING: return "XR_SESSION_LOSS_PENDING";
    case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
    case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
    case XR_ERROR_SESSION_LOST: return "XR_ERROR_SESSION_LOST";
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
    case XR_ERROR_FORM_FACTOR_UNSUPPORTED: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
    case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
    case XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING: return "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING";
    case XR_ERROR_GRAPHICS_DEVICE_INVALID: return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
    case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
    case XR_ERROR_CALL_ORDER_INVALID: return "XR_ERROR_CALL_ORDER_INVALID";
    case XR_ERROR_SESSION_NOT_RUNNING: return "XR_ERROR_SESSION_NOT_RUNNING";
    case XR_ERROR_SESSION_NOT_READY: return "XR_ERROR_SESSION_NOT_READY";
    case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED: return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";
    case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED (runtime present but could not start - headset not streaming?)";
    case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
    case XR_ERROR_OUT_OF_MEMORY: return "XR_ERROR_OUT_OF_MEMORY";
    case XR_ERROR_FUNCTION_UNSUPPORTED: return "XR_ERROR_FUNCTION_UNSUPPORTED";
    case XR_ERROR_FEATURE_UNSUPPORTED: return "XR_ERROR_FEATURE_UNSUPPORTED";
    case XR_ERROR_LIMIT_REACHED: return "XR_ERROR_LIMIT_REACHED";
    case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE (no OpenXR runtime registered / not running)";
    case XR_ERROR_NAME_INVALID: return "XR_ERROR_NAME_INVALID";
    case XR_ERROR_HANDLE_INVALID: return "XR_ERROR_HANDLE_INVALID";
    default: return "XR_ERROR (unmapped)";
    }
}
#define XRC(expr, what)                                                                   \
    do {                                                                                  \
        XrResult _r = (expr);                                                             \
        if (XR_FAILED(_r)) { LOGE("[xr] %s failed: %s (%d)", what, XrStr(_r), (int)_r); return false; } \
    } while (0)

inline Vec3  ToVec(const XrVector3f& v) { return {v.x, v.y, v.z}; }
inline Quat  ToQuat(const XrQuaternionf& q) { return {q.x, q.y, q.z, q.w}; }
inline VRFov ToFov(const XrFovf& f) { return {f.angleLeft, f.angleRight, f.angleUp, f.angleDown}; }
inline XrFovf FromFov(const VRFov& f) { return {f.angleLeft, f.angleRight, f.angleUp, f.angleDown}; }
inline XrPosef FromPose(const Pose& p) {
    XrPosef o{};
    o.orientation = {p.rot.x, p.rot.y, p.rot.z, p.rot.w};
    o.position = {p.pos.x, p.pos.y, p.pos.z};
    return o;
}

struct Swap {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> images;
    uint32_t w = 0, h = 0;
    int64_t  format = 0;
    uint32_t acquired = UINT32_MAX;

    void Destroy() {
        if (handle != XR_NULL_HANDLE) xrDestroySwapchain(handle);
        handle = XR_NULL_HANDLE; images.clear(); acquired = UINT32_MAX; w = h = 0;
    }
};

// ---- action set ---------------------------------------------------------------
struct Actions {
    XrActionSet set = XR_NULL_HANDLE;
    XrAction pose[2]{}, trigger[2]{}, squeeze[2]{}, stick[2]{}, stickClick[2]{}, haptic[2]{};
    XrAction btnA = XR_NULL_HANDLE, btnB = XR_NULL_HANDLE, btnX = XR_NULL_HANDLE,
             btnY = XR_NULL_HANDLE, btnMenu = XR_NULL_HANDLE;
    XrSpace  handSpace[2]{};
};

class OpenXRBackend final : public IVRBackend {
public:
    const char* Name() const override { return "openxr"; }

    // ---- attach: instance + system only (no graphics yet) ----
    bool TryAttach() override {
        std::lock_guard<std::recursive_mutex> g(g_mtx);
        if (instance_ != XR_NULL_HANDLE && systemId_ != XR_NULL_SYSTEM_ID) return true;
        // The worker calls this at PollHz (250 Hz). Creating an instance is expensive and
        // talks to the runtime over IPC, so only retry every couple of seconds.
        const uint64_t now = GetTickCount64();
        if (now - lastAttach_ < 2000) return false;
        lastAttach_ = now;
        if (!CreateInstance()) return false;
        if (!GetSystem()) return false;
        return true;
    }
    bool IsReady() const override { return instance_ != XR_NULL_HANDLE && systemId_ != XR_NULL_SYSTEM_ID; }

    void Detach() override {
        std::lock_guard<std::recursive_mutex> g(g_mtx);
        TeardownSession();
        if (instance_ != XR_NULL_HANDLE) { xrDestroyInstance(instance_); instance_ = XR_NULL_HANDLE; }
        systemId_ = XR_NULL_SYSTEM_ID;
    }

    // ---- graphics binding: creates the session, spaces, actions, eye swapchains ----
    bool BindGraphics(ID3D12Device* device, ID3D12CommandQueue* queue) override {
        std::lock_guard<std::recursive_mutex> g(g_mtx);
        if (bound_) return true;
        if (!IsReady() || !device || !queue) return false;

        // The runtime must be asked for its requirements before session creation, even
        // though we are handing it a device the game already made.
        auto pfnReq = GetProc<PFN_xrGetD3D12GraphicsRequirementsKHR>("xrGetD3D12GraphicsRequirementsKHR");
        if (!pfnReq) { LOGE("[xr] xrGetD3D12GraphicsRequirementsKHR missing (is XR_KHR_D3D12_enable supported?)"); return false; }
        XrGraphicsRequirementsD3D12KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        XRC(pfnReq(instance_, systemId_, &req), "xrGetD3D12GraphicsRequirementsKHR");

        LUID want = req.adapterLuid, have = device->GetAdapterLuid();
        if (want.LowPart != have.LowPart || want.HighPart != have.HighPart)
            LOGW("[xr] runtime prefers adapter LUID %08lx:%08lx but the game renders on %08lx:%08lx; "
                 "submission may be cross-adapter (slow)", want.HighPart, want.LowPart, have.HighPart, have.LowPart);

        XrGraphicsBindingD3D12KHR bind{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
        bind.device = device;
        bind.queue = queue;
        XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
        sci.next = &bind;
        sci.systemId = systemId_;
        XRC(xrCreateSession(instance_, &sci, &session_), "xrCreateSession");
        LOGI("[xr] session created on device %p queue %p", (void*)device, (void*)queue);

        if (!CreateSpaces()) return false;
        if (!CreateActions()) return false;
        device_ = device; queue_ = queue;
        bound_ = true;
        return true;
    }
    bool GraphicsBound() const override { return bound_; }

    bool GetRecommendedEyeSize(uint32_t& w, uint32_t& h) const override {
        if (viewConfig_.empty()) return false;
        w = viewConfig_[0].recommendedImageRectWidth;
        h = viewConfig_[0].recommendedImageRectHeight;
        return w > 0 && h > 0;
    }
    DXGI_FORMAT EyeFormat() const override { return (DXGI_FORMAT)eyeFormat_; }
    DXGI_FORMAT QuadFormat() const override { return (DXGI_FORMAT)quad_.format; }

    bool EnsureEyeSwapchains(uint32_t w, uint32_t h) override {
        std::lock_guard<std::recursive_mutex> g(g_mtx);
        if (!bound_ || w == 0 || h == 0) return false;
        if (!viewConfig_.empty()) {
            w = std::min(w, viewConfig_[0].maxImageRectWidth);
            h = std::min(h, viewConfig_[0].maxImageRectHeight);
        }
        if (eyes_[0].handle != XR_NULL_HANDLE) {
            // Only rebuild on a real change: the crop moves by a pixel or two as the runtime
            // re-predicts the eye poses, and tearing down swapchains every frame would stall.
            const uint32_t dw = eyes_[0].w > w ? eyes_[0].w - w : w - eyes_[0].w;
            const uint32_t dh = eyes_[0].h > h ? eyes_[0].h - h : h - eyes_[0].h;
            if (dw * 32 < eyes_[0].w && dh * 32 < eyes_[0].h) return true;
        }
        for (auto& e : eyes_) e.Destroy();
        if (eyeFormat_ == 0)
            eyeFormat_ = PickFormat({DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                     DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM});
        if (eyeFormat_ == 0) { LOGE("[xr] no usable eye swapchain format"); return false; }
        for (int i = 0; i < 2; ++i)
            if (!CreateSwap(eyes_[i], w, h, eyeFormat_)) return false;
        LOGI("[xr] eye swapchains %ux%u fmt %lld (%u images)", w, h, (long long)eyeFormat_,
             (unsigned)eyes_[0].images.size());
        return true;
    }

    bool EnsureQuadSwapchain(uint32_t w, uint32_t h, DXGI_FORMAT bbFormat) override {
        std::lock_guard<std::recursive_mutex> g(g_mtx);
        if (!bound_ || w == 0 || h == 0) return false;
        if (quad_.handle != XR_NULL_HANDLE && quad_.w == w && quad_.h == h) return true;
        quad_.Destroy();
        // Deliberately NOT "whatever is copy-compatible with the back buffer". A raw copy
        // preserves the bits, and the game's bits are gamma encoded while OpenXR reads a
        // non-sRGB swapchain as linear light - that mismatch is what washed the picture out.
        // The blitter converts in its shader instead, so the only thing that matters here is
        // precision: R16F first, so ELDEN RING's 10-bit output survives without banding.
        int64_t fmt = PickFormat({DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                  DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM});
        if (fmt == 0) { LOGE("[xr] runtime offers no usable quad format"); return false; }
        if (!CreateSwap(quad_, w, h, fmt)) return false;
        LOGI("[xr] quad swapchain %ux%u fmt %lld (back buffer fmt %u)", w, h, (long long)fmt, (unsigned)bbFormat);
        return true;
    }

    // ---- frame loop (render thread) ----
    bool WaitBeginFrame(VRFrameInfo& out) override {
        if (!bound_) return false;
        PumpEvents();
        if (!sessionRunning_) return false;

        XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XrResult r = xrWaitFrame(session_, &wi, &fs);   // deliberately outside the lock
        if (XR_FAILED(r)) { LOGW("[xr] xrWaitFrame: %s", XrStr(r)); return false; }

        std::lock_guard<std::recursive_mutex> g(g_mtx);
        XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
        r = xrBeginFrame(session_, &bi);
        if (XR_FAILED(r) && r != XR_FRAME_DISCARDED) { LOGW("[xr] xrBeginFrame: %s", XrStr(r)); return false; }

        displayTime_.store(fs.predictedDisplayTime);
        frameBegun_ = true;
        out.displayTimeNs = fs.predictedDisplayTime;
        out.displayTimeSec = (double)fs.predictedDisplayTime * 1e-9;
        out.shouldRender = fs.shouldRender != XR_FALSE;
        out.pairIndex = ++pairIndex_;
        return true;
    }

    bool LocateViews(int64_t displayTimeNs, VREyeView out[2]) override {
        return LocateIn(appSpace_, displayTimeNs, out);
    }
    // Locating the views in VIEW space gives the eyes relative to the head directly - no
    // second xrLocateSpace and no subtraction of two poses sampled at different instants.
    bool LocateEyeOffsets(int64_t displayTimeNs, VREyeView out[2]) override {
        return LocateIn(viewSpace_, displayTimeNs, out);
    }

    ID3D12Resource* AcquireEyeImage(VREye eye) override { return AcquireFrom(eyes_[eye]); }
    void ReleaseEyeImage(VREye eye) override { ReleaseTo(eyes_[eye]); }
    ID3D12Resource* AcquireQuadImage() override { return AcquireFrom(quad_); }
    void ReleaseQuadImage() override { ReleaseTo(quad_); }

    bool EndFrame(const VRSubmitLayer& layer) override {
        std::lock_guard<std::recursive_mutex> g(g_mtx);
        if (!bound_ || !frameBegun_) return false;
        frameBegun_ = false;

        XrCompositionLayerProjectionView pv[2]{};
        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        const XrCompositionLayerBaseHeader* layers[2]{};
        uint32_t count = 0;

        if (layer.projectionValid && eyes_[0].handle != XR_NULL_HANDLE) {
            for (int i = 0; i < 2; ++i) {
                pv[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                pv[i].pose = FromPose(layer.views[i].pose);
                pv[i].fov = FromFov(layer.views[i].fov);
                pv[i].subImage.swapchain = eyes_[i].handle;
                pv[i].subImage.imageArrayIndex = 0;
                pv[i].subImage.imageRect = {{0, 0}, {(int32_t)eyes_[i].w, (int32_t)eyes_[i].h}};
            }
            proj.space = appSpace_;
            proj.layerFlags = 0;   // the world is opaque; the game's alpha channel is not coverage
            proj.viewCount = 2;
            proj.views = pv;
            layers[count++] = (const XrCompositionLayerBaseHeader*)&proj;
        }
        if (layer.quadValid && quad_.handle != XR_NULL_HANDLE) {
            const float aspect = quad_.h ? (float)quad_.w / (float)quad_.h : 1.777f;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = quad_.handle;
            quad.subImage.imageArrayIndex = 0;
            quad.subImage.imageRect = {{0, 0}, {(int32_t)quad_.w, (int32_t)quad_.h}};
            quad.size = {layer.quadWidthM, layer.quadWidthM / aspect};
            // WORLD-locked screen with a lazy follow, not head-locked. The compositor can
            // reproject world-locked content perfectly at any submit rate, but view-locked
            // content must move WITH the head between submits - which reads as judder the
            // moment submissions run below the display rate (they always do here). The
            // screen re-anchors only when the head has turned/moved enough to lose it.
            XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
            bool world = false;
            if (XR_SUCCEEDED(xrLocateSpace(viewSpace_, appSpace_, displayTime_.load(), &loc)) &&
                (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
                const XrQuaternionf& q = loc.pose.orientation;
                // head forward = q * (0,0,-1), flattened to the horizon
                XrVector3f fwd{-2.f * (q.x * q.z + q.w * q.y),
                               0.f,
                               -(1.f - 2.f * (q.x * q.x + q.y * q.y))};
                const float len = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
                if (len > 0.2f) {   // not staring straight up/down
                    fwd.x /= len; fwd.z /= len;
                    const XrVector3f& hp = loc.pose.position;
                    const float dxa = hp.x - quadAnchorHead_.x, dza = hp.z - quadAnchorHead_.z;
                    const float dot = fwd.x * quadAnchorFwd_.x + fwd.z * quadAnchorFwd_.z;
                    if (!quadAnchorValid_ || layer.quadDistanceM != quadAnchorDist_ ||
                        dot < 0.75f /* ~41 deg */ || dxa * dxa + dza * dza > 0.5f * 0.5f ||
                        std::fabs(hp.y - quadAnchorHead_.y) > 0.5f) {
                        const float th = std::atan2(-fwd.x, -fwd.z);
                        quadAnchor_.orientation = {0.f, std::sin(th * 0.5f), 0.f, std::cos(th * 0.5f)};
                        quadAnchor_.position = {hp.x + fwd.x * layer.quadDistanceM,
                                                hp.y,
                                                hp.z + fwd.z * layer.quadDistanceM};
                        quadAnchorHead_ = hp;
                        quadAnchorFwd_ = fwd;
                        quadAnchorDist_ = layer.quadDistanceM;
                        quadAnchorValid_ = true;
                    }
                    quad.space = appSpace_;
                    quad.pose = quadAnchor_;
                    world = true;
                }
            }
            if (!world) {   // tracking lost: fall back to the old head-locked placement
                quadAnchorValid_ = false;
                quad.space = viewSpace_;
                quad.pose = {{0, 0, 0, 1}, {0, 0, -layer.quadDistanceM}};
            }
            layers[count++] = (const XrCompositionLayerBaseHeader*)&quad;
        }

        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = displayTime_.load();
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount = count;
        ei.layers = count ? layers : nullptr;
        XrResult r = xrEndFrame(session_, &ei);
        if (XR_FAILED(r)) {
            static XrResult s_last = XR_SUCCESS;
            if (s_last != r) { s_last = r; LOGW("[xr] xrEndFrame: %s", XrStr(r)); }
            return false;
        }
        return true;
    }

    // ---- pose/input plane (worker thread) ----
    bool Poll(VRTracking& tr, VRInputState& in) override {
        std::lock_guard<std::recursive_mutex> g(g_mtx);
        if (session_ == XR_NULL_HANDLE || !actions_.set) return false;
        if (sessionState_ != XR_SESSION_STATE_FOCUSED && sessionState_ != XR_SESSION_STATE_VISIBLE) return false;

        XrActiveActionSet active{actions_.set, XR_NULL_PATH};
        XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
        si.countActiveActionSets = 1;
        si.activeActionSets = &active;
        XrResult r = xrSyncActions(session_, &si);
        if (XR_FAILED(r)) return false;
        if (r == XR_SESSION_NOT_FOCUSED) return false;

        const XrTime t = SampleTime();
        tr.time = (double)t * 1e-9;
        tr.head = LocateSpace(viewSpace_, t);
        for (int h = 0; h < 2; ++h) {
            tr.hand[h] = LocateSpace(actions_.handSpace[h], t);
            in.indexTrigger[h] = GetFloat(actions_.trigger[h]);
            in.handTrigger[h]  = GetFloat(actions_.squeeze[h]);
            XrVector2f v = GetVec2(actions_.stick[h]);
            in.thumb[h][0] = v.x;
            in.thumb[h][1] = v.y;
            in.controllerConnected[h] = tr.hand[h].valid || in.indexTrigger[h] > 0.01f ||
                                        in.handTrigger[h] > 0.01f || std::fabs(v.x) > 0.01f || std::fabs(v.y) > 0.01f;
        }
        uint32_t b = 0;
        if (GetBool(actions_.btnA)) b |= VRB_A;
        if (GetBool(actions_.btnB)) b |= VRB_B;
        if (GetBool(actions_.btnX)) b |= VRB_X;
        if (GetBool(actions_.btnY)) b |= VRB_Y;
        if (GetBool(actions_.btnMenu)) b |= VRB_Menu;
        if (GetBool(actions_.stickClick[HandLeft]))  b |= VRB_LThumb;
        if (GetBool(actions_.stickClick[HandRight])) b |= VRB_RThumb;
        in.buttons = b;

        static double s_lastDump = 0;
        if (tr.time - s_lastDump > 2.0) {
            s_lastDump = tr.time;
            LOGD("[xr] raw: state %d conn(%d,%d) btn 0x%04x trig(%.2f,%.2f) grip(%.2f,%.2f) "
                 "stick L(%.2f,%.2f) R(%.2f,%.2f) head(%.2f,%.2f,%.2f)",
                 (int)sessionState_, in.controllerConnected[0], in.controllerConnected[1], b,
                 in.indexTrigger[0], in.indexTrigger[1], in.handTrigger[0], in.handTrigger[1],
                 in.thumb[0][0], in.thumb[0][1], in.thumb[1][0], in.thumb[1][1],
                 tr.head.pos.x, tr.head.pos.y, tr.head.pos.z);
        }
        return true;
    }

    void Haptic(Hand hand, float amplitude, float durationSec) override {
        std::lock_guard<std::recursive_mutex> g(g_mtx);
        if (session_ == XR_NULL_HANDLE || !actions_.haptic[hand]) return;
        XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
        hi.action = actions_.haptic[hand];
        if (amplitude <= 0.f) { xrStopHapticFeedback(session_, &hi); return; }
        XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
        v.amplitude = Clamp(amplitude, 0.f, 1.f);
        v.duration = durationSec > 0 ? (XrDuration)(durationSec * 1e9) : XR_MIN_HAPTIC_DURATION;
        v.frequency = XR_FREQUENCY_UNSPECIFIED;
        xrApplyHapticFeedback(session_, &hi, (const XrHapticBaseHeader*)&v);
    }

private:
    bool LocateIn(XrSpace space, int64_t displayTimeNs, VREyeView out[2]) {
        std::lock_guard<std::recursive_mutex> g(g_mtx);
        if (!bound_ || space == XR_NULL_HANDLE) return false;
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = displayTimeNs;
        li.space = space;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t n = 0;
        XrResult r = xrLocateViews(session_, &li, &vs, 2, &n, views);
        if (XR_FAILED(r) || n < 2) return false;
        const bool ok = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
                        (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
        for (int i = 0; i < 2; ++i) {
            out[i].pose.pos = ToVec(views[i].pose.position);
            out[i].pose.rot = ToQuat(views[i].pose.orientation);
            out[i].pose.valid = ok;
            out[i].fov = ToFov(views[i].fov);
        }
        return ok;
    }

    // ---- setup helpers ----
    template <class T> T GetProc(const char* name) const {
        PFN_xrVoidFunction fn = nullptr;
        if (XR_FAILED(xrGetInstanceProcAddr(instance_, name, &fn))) return nullptr;
        return reinterpret_cast<T>(fn);
    }

    bool CreateInstance() {
        uint32_t n = 0;
        if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr))) return false;
        std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
        if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data()))) return false;
        auto has = [&](const char* e) {
            for (auto& p : props) if (strcmp(p.extensionName, e) == 0) return true;
            return false;
        };
        if (!has(XR_KHR_D3D12_ENABLE_EXTENSION_NAME)) {
            static bool warned = false;
            if (!warned) { warned = true; LOGE("[xr] runtime does not support %s", XR_KHR_D3D12_ENABLE_EXTENSION_NAME); }
            return false;
        }
        std::vector<const char*> exts{XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
        haveTimeConv_ = has(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
        if (haveTimeConv_) exts.push_back(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
        if (Cfg().vrXrDebug && has(XR_EXT_DEBUG_UTILS_EXTENSION_NAME)) exts.push_back(XR_EXT_DEBUG_UTILS_EXTENSION_NAME);

        XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(ci.applicationInfo.applicationName, "ERVR");
        strcpy_s(ci.applicationInfo.engineName, "ERVR");
        ci.applicationInfo.applicationVersion = 1;
        // Request 1.0, NOT XR_CURRENT_API_VERSION. The 1.1 SDK makes that macro 1.1, and
        // runtimes that implement only OpenXR 1.0 (SteamVR, some VDXR builds) reject a 1.1
        // instance with XR_ERROR_INITIALIZATION_FAILED (-4). d2vr proves 1.0 works here.
        ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        ci.enabledExtensionCount = (uint32_t)exts.size();
        ci.enabledExtensionNames = exts.data();
        XrResult r = xrCreateInstance(&ci, &instance_);
        if (XR_FAILED(r)) {
            static XrResult s_last = XR_SUCCESS;
            if (s_last != r) {
                s_last = r;
                LOGW("[xr] xrCreateInstance failed: %s (%d). The runtime is registered but not accepting "
                     "connections - with Virtual Desktop/VDXR the headset must be CONNECTED AND STREAMING "
                     "before the game starts. Retrying every 2 s.", XrStr(r), (int)r);
            }
            instance_ = XR_NULL_HANDLE;
            return false;
        }
        XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_SUCCEEDED(xrGetInstanceProperties(instance_, &ip)))
            LOGI("[xr] runtime \"%s\" %u.%u.%u", ip.runtimeName,
                 XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
        return true;
    }

    bool GetSystem() {
        XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
        gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrResult r = xrGetSystem(instance_, &gi, &systemId_);
        if (XR_FAILED(r)) { systemId_ = XR_NULL_SYSTEM_ID; return false; }   // headset off: retry later, quietly

        uint32_t n = 0;
        xrEnumerateViewConfigurationViews(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
        viewConfig_.assign(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        XRC(xrEnumerateViewConfigurationViews(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                              n, &n, viewConfig_.data()), "xrEnumerateViewConfigurationViews");
        if (viewConfig_.size() < 2) { LOGE("[xr] stereo view config unavailable"); return false; }
        XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_SUCCEEDED(xrGetSystemProperties(instance_, systemId_, &sp)))
            LOGI("[xr] system \"%s\": recommended eye %ux%u, max %ux%u", sp.systemName,
                 viewConfig_[0].recommendedImageRectWidth, viewConfig_[0].recommendedImageRectHeight,
                 viewConfig_[0].maxImageRectWidth, viewConfig_[0].maxImageRectHeight);
        return true;
    }

    bool CreateSpaces() {
        XrReferenceSpaceCreateInfo ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        ci.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
        ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        if (XR_FAILED(xrCreateReferenceSpace(session_, &ci, &appSpace_))) {
            ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            XRC(xrCreateReferenceSpace(session_, &ci, &appSpace_), "xrCreateReferenceSpace(LOCAL)");
            LOGI("[xr] reference space: LOCAL (STAGE unavailable)");
        } else {
            LOGI("[xr] reference space: STAGE");
        }
        ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        XRC(xrCreateReferenceSpace(session_, &ci, &viewSpace_), "xrCreateReferenceSpace(VIEW)");
        return true;
    }

    XrPath Path(const char* s) const {
        XrPath p = XR_NULL_PATH;
        xrStringToPath(instance_, s, &p);
        return p;
    }
    bool MakeAction(XrAction& a, const char* name, const char* localized, XrActionType type) {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(ci.actionName, name);
        strcpy_s(ci.localizedActionName, localized);
        ci.actionType = type;
        XRC(xrCreateAction(actions_.set, &ci, &a), name);
        return true;
    }

    bool CreateActions() {
        XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(si.actionSetName, "ervr");
        strcpy_s(si.localizedActionSetName, "ERVR");
        si.priority = 0;
        XRC(xrCreateActionSet(instance_, &si, &actions_.set), "xrCreateActionSet");

        const char* side[2] = {"left", "right"};
        for (int h = 0; h < 2; ++h) {
            char n[64];
            sprintf_s(n, "hand_pose_%s", side[h]);   if (!MakeAction(actions_.pose[h], n, n, XR_ACTION_TYPE_POSE_INPUT)) return false;
            sprintf_s(n, "trigger_%s", side[h]);     if (!MakeAction(actions_.trigger[h], n, n, XR_ACTION_TYPE_FLOAT_INPUT)) return false;
            sprintf_s(n, "squeeze_%s", side[h]);     if (!MakeAction(actions_.squeeze[h], n, n, XR_ACTION_TYPE_FLOAT_INPUT)) return false;
            sprintf_s(n, "stick_%s", side[h]);       if (!MakeAction(actions_.stick[h], n, n, XR_ACTION_TYPE_VECTOR2F_INPUT)) return false;
            sprintf_s(n, "stick_click_%s", side[h]); if (!MakeAction(actions_.stickClick[h], n, n, XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;
            sprintf_s(n, "haptic_%s", side[h]);      if (!MakeAction(actions_.haptic[h], n, n, XR_ACTION_TYPE_VIBRATION_OUTPUT)) return false;
        }
        if (!MakeAction(actions_.btnA, "btn_a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;
        if (!MakeAction(actions_.btnB, "btn_b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;
        if (!MakeAction(actions_.btnX, "btn_x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;
        if (!MakeAction(actions_.btnY, "btn_y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;
        if (!MakeAction(actions_.btnMenu, "btn_menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT)) return false;

        SuggestTouch();
        SuggestIndex();
        SuggestSimple();

        for (int h = 0; h < 2; ++h) {
            XrActionSpaceCreateInfo asi{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            asi.action = actions_.pose[h];
            asi.poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}};
            XRC(xrCreateActionSpace(session_, &asi, &actions_.handSpace[h]), "xrCreateActionSpace");
        }
        XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        ai.countActionSets = 1;
        ai.actionSets = &actions_.set;
        XRC(xrAttachSessionActionSets(session_, &ai), "xrAttachSessionActionSets");
        return true;
    }

    void Suggest(const char* profile, std::vector<XrActionSuggestedBinding> b) {
        XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        s.interactionProfile = Path(profile);
        s.countSuggestedBindings = (uint32_t)b.size();
        s.suggestedBindings = b.data();
        XrResult r = xrSuggestInteractionProfileBindings(instance_, &s);
        if (XR_FAILED(r)) LOGW("[xr] bindings for %s rejected: %s", profile, XrStr(r));
    }
    void SuggestTouch() {
        Suggest("/interaction_profiles/oculus/touch_controller", {
            {actions_.pose[0], Path("/user/hand/left/input/grip/pose")},
            {actions_.pose[1], Path("/user/hand/right/input/grip/pose")},
            {actions_.trigger[0], Path("/user/hand/left/input/trigger/value")},
            {actions_.trigger[1], Path("/user/hand/right/input/trigger/value")},
            {actions_.squeeze[0], Path("/user/hand/left/input/squeeze/value")},
            {actions_.squeeze[1], Path("/user/hand/right/input/squeeze/value")},
            {actions_.stick[0], Path("/user/hand/left/input/thumbstick")},
            {actions_.stick[1], Path("/user/hand/right/input/thumbstick")},
            {actions_.stickClick[0], Path("/user/hand/left/input/thumbstick/click")},
            {actions_.stickClick[1], Path("/user/hand/right/input/thumbstick/click")},
            {actions_.btnX, Path("/user/hand/left/input/x/click")},
            {actions_.btnY, Path("/user/hand/left/input/y/click")},
            {actions_.btnA, Path("/user/hand/right/input/a/click")},
            {actions_.btnB, Path("/user/hand/right/input/b/click")},
            {actions_.btnMenu, Path("/user/hand/left/input/menu/click")},
            {actions_.haptic[0], Path("/user/hand/left/output/haptic")},
            {actions_.haptic[1], Path("/user/hand/right/output/haptic")},
        });
    }
    void SuggestIndex() {
        Suggest("/interaction_profiles/valve/index_controller", {
            {actions_.pose[0], Path("/user/hand/left/input/grip/pose")},
            {actions_.pose[1], Path("/user/hand/right/input/grip/pose")},
            {actions_.trigger[0], Path("/user/hand/left/input/trigger/value")},
            {actions_.trigger[1], Path("/user/hand/right/input/trigger/value")},
            {actions_.squeeze[0], Path("/user/hand/left/input/squeeze/value")},
            {actions_.squeeze[1], Path("/user/hand/right/input/squeeze/value")},
            {actions_.stick[0], Path("/user/hand/left/input/thumbstick")},
            {actions_.stick[1], Path("/user/hand/right/input/thumbstick")},
            {actions_.stickClick[0], Path("/user/hand/left/input/thumbstick/click")},
            {actions_.stickClick[1], Path("/user/hand/right/input/thumbstick/click")},
            {actions_.btnX, Path("/user/hand/left/input/a/click")},
            {actions_.btnY, Path("/user/hand/left/input/b/click")},
            {actions_.btnA, Path("/user/hand/right/input/a/click")},
            {actions_.btnB, Path("/user/hand/right/input/b/click")},
            {actions_.btnMenu, Path("/user/hand/left/input/system/click")},
            {actions_.haptic[0], Path("/user/hand/left/output/haptic")},
            {actions_.haptic[1], Path("/user/hand/right/output/haptic")},
        });
    }
    void SuggestSimple() {
        Suggest("/interaction_profiles/khr/simple_controller", {
            {actions_.pose[0], Path("/user/hand/left/input/grip/pose")},
            {actions_.pose[1], Path("/user/hand/right/input/grip/pose")},
            {actions_.btnA, Path("/user/hand/right/input/select/click")},
            {actions_.btnX, Path("/user/hand/left/input/select/click")},
            {actions_.btnMenu, Path("/user/hand/left/input/menu/click")},
            {actions_.haptic[0], Path("/user/hand/left/output/haptic")},
            {actions_.haptic[1], Path("/user/hand/right/output/haptic")},
        });
    }

    // First of `wanted` that the runtime offers; 0 if none.
    int64_t PickFormat(std::vector<int64_t> wanted) const {
        uint32_t n = 0;
        if (XR_FAILED(xrEnumerateSwapchainFormats(session_, 0, &n, nullptr)) || n == 0) return 0;
        std::vector<int64_t> have(n);
        if (XR_FAILED(xrEnumerateSwapchainFormats(session_, n, &n, have.data()))) return 0;
        for (int64_t w : wanted)
            if (w != 0 && std::find(have.begin(), have.end(), w) != have.end()) return w;
        return 0;
    }

    bool CreateSwap(Swap& s, uint32_t w, uint32_t h, int64_t format) {
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = format;
        ci.sampleCount = 1;
        ci.width = w; ci.height = h;
        ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
        XRC(xrCreateSwapchain(session_, &ci, &s.handle), "xrCreateSwapchain");
        uint32_t n = 0;
        XRC(xrEnumerateSwapchainImages(s.handle, 0, &n, nullptr), "xrEnumerateSwapchainImages");
        s.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        XRC(xrEnumerateSwapchainImages(s.handle, n, &n, (XrSwapchainImageBaseHeader*)s.images.data()),
            "xrEnumerateSwapchainImages");
        s.w = w; s.h = h; s.format = format;
        return true;
    }

    ID3D12Resource* AcquireFrom(Swap& s) {
        std::lock_guard<std::recursive_mutex> g(g_mtx);
        if (s.handle == XR_NULL_HANDLE || s.acquired != UINT32_MAX) return nullptr;
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(s.handle, &ai, &idx))) return nullptr;
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = 100000000;   // 100 ms; never block the render thread indefinitely
        if (XR_FAILED(xrWaitSwapchainImage(s.handle, &wi))) {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(s.handle, &ri);
            return nullptr;
        }
        s.acquired = idx;
        return s.images[idx].texture;
    }
    void ReleaseTo(Swap& s) {
        std::lock_guard<std::recursive_mutex> g(g_mtx);
        if (s.handle == XR_NULL_HANDLE || s.acquired == UINT32_MAX) return;
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(s.handle, &ri);
        s.acquired = UINT32_MAX;
    }

    XrTime SampleTime() {
        XrTime t = displayTime_.load();
        if (t != 0) return t;
        if (haveTimeConv_) {
            auto pfn = GetProc<PFN_xrConvertWin32PerformanceCounterToTimeKHR>("xrConvertWin32PerformanceCounterToTimeKHR");
            LARGE_INTEGER now;
            if (pfn && QueryPerformanceCounter(&now) && XR_SUCCEEDED(pfn(instance_, &now, &t))) return t;
        }
        return 0;
    }

    Pose LocateSpace(XrSpace space, XrTime t) {
        Pose out;
        if (space == XR_NULL_HANDLE || appSpace_ == XR_NULL_HANDLE || t == 0) return out;
        XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION, &vel};
        if (XR_FAILED(xrLocateSpace(space, appSpace_, t, &loc))) return out;
        if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return out;
        out.rot = ToQuat(loc.pose.orientation);
        out.pos = ToVec(loc.pose.position);
        if (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)  out.linVel = ToVec(vel.linearVelocity);
        if (vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) out.angVel = ToVec(vel.angularVelocity);
        out.valid = true;
        return out;
    }

    bool GetBool(XrAction a) {
        if (!a) return false;
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xrGetActionStateBoolean(session_, &gi, &s)) && s.isActive && s.currentState;
    }
    float GetFloat(XrAction a) {
        if (!a) return 0.f;
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_FAILED(xrGetActionStateFloat(session_, &gi, &s)) || !s.isActive) return 0.f;
        return s.currentState;
    }
    XrVector2f GetVec2(XrAction a) {
        if (!a) return {0, 0};
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_FAILED(xrGetActionStateVector2f(session_, &gi, &s)) || !s.isActive) return {0, 0};
        return s.currentState;
    }

    void PumpEvents() {
        std::lock_guard<std::recursive_mutex> g(g_mtx);
        for (;;) {
            XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
            if (xrPollEvent(instance_, &ev) != XR_SUCCESS) break;
            switch (ev.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                auto* e = (XrEventDataSessionStateChanged*)&ev;
                sessionState_ = e->state;
                LOGI("[xr] session state -> %d", (int)e->state);
                if (e->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (XR_SUCCEEDED(xrBeginSession(session_, &bi))) { sessionRunning_ = true; LOGI("[xr] session running"); }
                } else if (e->state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session_);
                    sessionRunning_ = false;
                    frameBegun_ = false;
                } else if (e->state == XR_SESSION_STATE_EXITING || e->state == XR_SESSION_STATE_LOSS_PENDING) {
                    TeardownSession();
                    return;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                LOGW("[xr] instance loss pending; tearing down");
                TeardownSession();
                if (instance_ != XR_NULL_HANDLE) { xrDestroyInstance(instance_); instance_ = XR_NULL_HANDLE; }
                systemId_ = XR_NULL_SYSTEM_ID;
                return;
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                LOGD("[xr] interaction profile changed");
                break;
            default: break;
            }
        }
    }

    void TeardownSession() {
        std::lock_guard<std::recursive_mutex> g(g_mtx);
        for (auto& e : eyes_) e.Destroy();
        quad_.Destroy();
        for (auto& s : actions_.handSpace) if (s) { xrDestroySpace(s); s = XR_NULL_HANDLE; }
        if (actions_.set) { xrDestroyActionSet(actions_.set); actions_ = Actions{}; }
        if (appSpace_ != XR_NULL_HANDLE)  { xrDestroySpace(appSpace_);  appSpace_ = XR_NULL_HANDLE; }
        if (viewSpace_ != XR_NULL_HANDLE) { xrDestroySpace(viewSpace_); viewSpace_ = XR_NULL_HANDLE; }
        if (session_ != XR_NULL_HANDLE)   { xrDestroySession(session_); session_ = XR_NULL_HANDLE; }
        sessionRunning_ = false;
        frameBegun_ = false;
        bound_ = false;
        sessionState_ = XR_SESSION_STATE_UNKNOWN;
        device_ = nullptr; queue_ = nullptr;
        displayTime_.store(0);
    }

    XrInstance  instance_ = XR_NULL_HANDLE;
    XrSystemId  systemId_ = XR_NULL_SYSTEM_ID;
    XrSession   session_ = XR_NULL_HANDLE;
    XrSpace     appSpace_ = XR_NULL_HANDLE, viewSpace_ = XR_NULL_HANDLE;
    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
    std::vector<XrViewConfigurationView> viewConfig_;
    Swap        eyes_[2], quad_;
    // Lazy world anchor for the cinema quad (render thread only).
    bool        quadAnchorValid_ = false;
    XrPosef     quadAnchor_{};
    XrVector3f  quadAnchorHead_{};
    XrVector3f  quadAnchorFwd_{};
    float       quadAnchorDist_ = 0.f;
    int64_t     eyeFormat_ = 0;
    Actions     actions_;
    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    bool bound_ = false, sessionRunning_ = false, frameBegun_ = false, haveTimeConv_ = false;
    uint64_t lastAttach_ = 0;
    std::atomic<XrTime> displayTime_{0};
    uint64_t pairIndex_ = 0;
};

} // namespace

IVRBackend* CreateOpenXRBackend() { static OpenXRBackend b; return &b; }
}
