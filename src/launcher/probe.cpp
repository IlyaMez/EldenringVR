// The headset check: this launcher started again as `--probe` (HeadsetCheck), with XR_RUNTIME_JSON naming the
// runtime to ask (unset: Windows' active one). It opens an OpenXR instance with no graphics at all, asks for a
// head-mounted system and prints what answered, one "label: value" line each (text::parseProbe). A process of
// its own, so each check loads the runtime fresh and a runtime that hangs is simply ended.
#include "system.hpp"
#include <openxr/openxr.h>
#include <cstdio>
#include <cstring>

namespace launcher {
namespace {

void print(const std::string& line) {
    const std::string text = line + "\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

std::string failure(XrResult result) {
    switch (result) {
    case XR_ERROR_RUNTIME_UNAVAILABLE: return "no OpenXR runtime is installed, or it is not available";
    case XR_ERROR_RUNTIME_FAILURE:
        return "the runtime is installed but refused to start (Virtual Desktop: connect and stream first)";
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "no headset is connected to this runtime";
    case XR_ERROR_INITIALIZATION_FAILED: return "the runtime failed to initialize";
    default: return "the runtime answered with code " + std::to_string(static_cast<int>(result));
    }
}

} // namespace

int runProbe() {
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(create.applicationInfo.applicationName, "ERVR Launcher headset check");
    strcpy_s(create.applicationInfo.engineName, "ERVR");
    create.applicationInfo.applicationVersion = 1;
    // 1.0, as ERVR itself asks: runtimes that only implement 1.0 refuse a 1.1 instance.
    create.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    XrInstance instance = XR_NULL_HANDLE;
    if (const XrResult r = xrCreateInstance(&create, &instance); XR_FAILED(r)) {
        print("error: " + failure(r));
        return 3;
    }
    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &properties))) {
        char line[XR_MAX_RUNTIME_NAME_SIZE + 64];
        std::snprintf(line, sizeof(line), "runtime: %s %u.%u.%u", properties.runtimeName,
                      XR_VERSION_MAJOR(properties.runtimeVersion), XR_VERSION_MINOR(properties.runtimeVersion),
                      XR_VERSION_PATCH(properties.runtimeVersion));
        print(line);
    }
    XrSystemGetInfo get{XR_TYPE_SYSTEM_GET_INFO};
    get.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    if (const XrResult r = xrGetSystem(instance, &get, &system); XR_FAILED(r)) {
        print("error: " + failure(r));
        xrDestroyInstance(instance);
        return 2;
    }
    XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(instance, system, &systemProperties)))
        print(std::string("headset: ") + systemProperties.systemName);
    uint32_t count = 0;
    XrViewConfigurationView views[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                                       &count, views)) &&
        count >= 1)
        print("eye: " + std::to_string(views[0].recommendedImageRectWidth) + "x" +
              std::to_string(views[0].recommendedImageRectHeight));
    xrDestroyInstance(instance);
    return 0;
}

} // namespace launcher
