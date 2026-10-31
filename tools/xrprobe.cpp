// xrprobe - standalone OpenXR/D3D12 diagnostic for ERVR.
//
// Answers, without launching ELDEN RING:
//   * does xrCreateInstance actually succeed right now (and if not, with what error)?
//   * which runtime/system is active, and what per-eye resolution does it want?
//   * exactly which swapchain formats does the runtime offer?
//   * is any of them copy-compatible with ER's R10G10B10A2_UNORM back buffer, or do we
//     need a shader blit for the cinema/eye copy?
//
// Run it with the headset on and streaming, the same way you would start the game.
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdio>
#include <vector>

static const char* FmtName(int64_t f) {
    switch ((DXGI_FORMAT)f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:        return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM:        return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R10G10B10A2_UNORM:     return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R11G11B10_FLOAT:       return "R11G11B10_FLOAT";
    case DXGI_FORMAT_D32_FLOAT:             return "D32_FLOAT";
    case DXGI_FORMAT_D24_UNORM_S8_UINT:     return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_D16_UNORM:             return "D16_UNORM";
    default:                                return "(other)";
    }
}
static DXGI_FORMAT Family(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    default: return f;
    }
}

int main() {
    printf("=== ERVR OpenXR probe ===\n\n");

    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());
    bool haveD3D12 = false;
    printf("[1] runtime advertises %u extensions\n", extCount);
    for (auto& e : exts) {
        printf("    %s v%u\n", e.extensionName, e.extensionVersion);
        if (strcmp(e.extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME) == 0) haveD3D12 = true;
    }
    printf("    XR_KHR_D3D12_enable: %s\n\n", haveD3D12 ? "YES" : "NO  <-- fatal");
    if (!haveD3D12) return 1;

    const char* want[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "ERVR probe");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;   // 1.1 is rejected by 1.0-only runtimes
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = want;
    // VDXR refuses an instance until the headset is actually streaming, so poll for a
    // while instead of failing instantly: start this, then connect the headset and watch.
    XrInstance inst = XR_NULL_HANDLE;
    XrResult r = XR_ERROR_RUNTIME_FAILURE;
    for (int attempt = 0; attempt < 30; ++attempt) {
        r = xrCreateInstance(&ici, &inst);
        if (XR_SUCCEEDED(r)) break;
        if (attempt == 0)
            printf("[2] xrCreateInstance -> %d (not ready). Waiting up to 60 s - connect the headset now.\n"
                   "    (Virtual Desktop: put the headset on, open the Virtual Desktop app, connect to this PC.)\n",
                   (int)r);
        printf("    retry %2d/30 ...\r", attempt + 1);
        fflush(stdout);
        Sleep(2000);
    }
    printf("\n[2] xrCreateInstance -> %d %s\n", (int)r, XR_SUCCEEDED(r) ? "(OK)" : "(FAILED)");
    if (XR_FAILED(r)) {
        printf("    Gave up. The runtime is registered but never accepted a connection.\n"
               "    -4 = XR_ERROR_INITIALIZATION_FAILED, which for VDXR means 'no active streaming session'.\n");
        return 1;
    }
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(inst, &ip);
    printf("    runtime: \"%s\" %u.%u.%u\n\n", ip.runtimeName,
           XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    r = xrGetSystem(inst, &sgi, &sys);
    printf("[3] xrGetSystem -> %d %s\n", (int)r, XR_SUCCEEDED(r) ? "(OK)" : "(FAILED - headset off?)");
    if (XR_FAILED(r)) return 1;
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(inst, sys, &sp);
    uint32_t vcount = 0;
    xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &vcount, nullptr);
    std::vector<XrViewConfigurationView> views(vcount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, vcount, &vcount, views.data());
    printf("    system: \"%s\"\n", sp.systemName);
    if (vcount) printf("    recommended per-eye: %ux%u   max: %ux%u\n\n",
                       views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight,
                       views[0].maxImageRectWidth, views[0].maxImageRectHeight);

    auto pfnReq = (PFN_xrGetD3D12GraphicsRequirementsKHR)nullptr;
    xrGetInstanceProcAddr(inst, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&pfnReq);
    XrGraphicsRequirementsD3D12KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    if (pfnReq) pfnReq(inst, sys, &req);
    printf("[4] runtime wants adapter LUID %08lx:%08lx, feature level %d\n",
           req.adapterLuid.HighPart, req.adapterLuid.LowPart, (int)req.minFeatureLevel);

    IDXGIFactory4* factory = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    IDXGIAdapter1* adapter = nullptr;
    if (factory) factory->EnumAdapterByLuid(req.adapterLuid, IID_PPV_ARGS(&adapter));
    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(adapter, req.minFeatureLevel ? req.minFeatureLevel : D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
    printf("    D3D12CreateDevice -> 0x%08lx %s\n\n", hr, SUCCEEDED(hr) ? "(OK)" : "(FAILED)");
    if (FAILED(hr)) return 1;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

    XrGraphicsBindingD3D12KHR bind{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    bind.device = dev; bind.queue = queue;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &bind; sci.systemId = sys;
    XrSession session = XR_NULL_HANDLE;
    r = xrCreateSession(inst, &sci, &session);
    printf("[5] xrCreateSession -> %d %s\n\n", (int)r, XR_SUCCEEDED(r) ? "(OK)" : "(FAILED)");
    if (XR_FAILED(r)) return 1;

    uint32_t n = 0;
    xrEnumerateSwapchainFormats(session, 0, &n, nullptr);
    std::vector<int64_t> fmts(n);
    xrEnumerateSwapchainFormats(session, n, &n, fmts.data());
    printf("[6] runtime offers %u swapchain formats:\n", n);
    bool hasER = false, hasCompat = false;
    const DXGI_FORMAT erFmt = DXGI_FORMAT_R10G10B10A2_UNORM;   // ELDEN RING's back buffer
    for (int64_t f : fmts) {
        bool compat = Family((DXGI_FORMAT)f) == Family(erFmt);
        if ((DXGI_FORMAT)f == erFmt) hasER = true;
        if (compat) hasCompat = true;
        printf("      %4lld  %-22s%s\n", (long long)f, FmtName(f), compat ? "  <-- copy-compatible with ER" : "");
    }
    printf("\n=== VERDICT ===\n");
    printf("  ER back buffer            : %d %s\n", (int)erFmt, FmtName(erFmt));
    printf("  exact match offered       : %s\n", hasER ? "YES" : "no");
    printf("  copy-compatible offered   : %s\n", hasCompat ? "YES" : "no");
    printf("  -> cinema mode needs      : %s\n",
           hasCompat ? "nothing, plain CopyResource works" : "a SHADER BLIT (format conversion)");

    xrDestroySession(session);
    xrDestroyInstance(inst);
    if (queue) queue->Release();
    if (dev) dev->Release();
    if (adapter) adapter->Release();
    if (factory) factory->Release();
    return 0;
}
