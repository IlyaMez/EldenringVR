// ERVR Launcher: finds ELDEN RING, the VR runtime and what else sits in the game folder, installs ERVR there,
// starts the game with EAC off with one button and shows ERVR's log as it plays. `--probe` runs the headset
// check instead (probe.cpp) and prints its answer.
#include "ui.hpp"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <filesystem>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
ID3D11Device* device{};
ID3D11DeviceContext* context{};
IDXGISwapChain* swapChain{};
ID3D11RenderTargetView* target{};
UINT resizeWidth{}, resizeHeight{};
float dpiScale = 1;
bool dpiChanged{};
constexpr wchar_t kClass[] = L"ERVRLauncher";
// The window's ground (ui.cpp's kGround).
constexpr COLORREF kGround = RGB(12, 11, 9);

bool createDevice(HWND window) {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level{};
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                                   D3D11_SDK_VERSION, &desc, &swapChain, &device, &level, &context);
    if (FAILED(result)) // e.g. a remote desktop session
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                               &desc, &swapChain, &device, &level, &context);
    return SUCCEEDED(result);
}

void createTarget() {
    ID3D11Texture2D* buffer{};
    if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) {
        device->CreateRenderTargetView(buffer, nullptr, &target);
        buffer->Release();
    }
}

void releaseTarget() {
    if (target) {
        target->Release();
        target = nullptr;
    }
}

void releaseDevice() {
    releaseTarget();
    if (swapChain) swapChain->Release();
    if (context) context->Release();
    if (device) device->Release();
    swapChain = nullptr;
    context = nullptr;
    device = nullptr;
}

void applyStyle(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    launcher::theme(style);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

launcher::Fonts loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    wchar_t windows[MAX_PATH];
    GetWindowsDirectoryW(windows, MAX_PATH);
    const std::filesystem::path folder = std::filesystem::path(windows) / L"Fonts";
    const auto file = [&](const wchar_t* name) {
        const auto path = folder / name;
        std::error_code error;
        return std::filesystem::is_regular_file(path, error) ? launcher::narrow(path.wstring()) : std::string();
    };
    std::string icons = file(L"SegoeIcons.ttf"); // Windows 11
    if (icons.empty())
        icons = file(L"segmdl2.ttf"); // Windows 10
    const std::string segoe = file(L"segoeui.ttf");
    launcher::Fonts fonts;
    fonts.icons = !icons.empty();
    // The icons, and Segoe UI for what the font lacks (arrows, the menu glyph), merged into it.
    const auto merge = [&](ImFont* font, float size) {
        if (!font)
            return font;
        ImFontConfig config;
        config.MergeMode = true;
        if (fonts.icons) {
            config.GlyphOffset = ImVec2(0, 2);
            io.Fonts->AddFontFromFileTTF(icons.c_str(), size, &config);
        }
        if (!segoe.empty()) {
            config.GlyphOffset = ImVec2(0, 0);
            io.Fonts->AddFontFromFileTTF(segoe.c_str(), size, &config);
        }
        return font;
    };
    // The launcher's own print (launcher.rc); Windows' own fonts where one is missing.
    const auto load = [&](const wchar_t* resource, const wchar_t* fallback, float size) -> ImFont* {
        ImFont* font = nullptr;
        if (const HRSRC found = resource ? FindResourceW(nullptr, resource, RT_RCDATA) : nullptr) {
            void* bytes = LockResource(LoadResource(nullptr, found));
            ImFontConfig config;
            config.FontDataOwnedByAtlas = false; // the bytes are the module's
            if (bytes)
                font = io.Fonts->AddFontFromMemoryTTF(bytes, static_cast<int>(SizeofResource(nullptr, found)), size,
                                                      &config);
        }
        if (const std::string path = file(fallback); !font && !path.empty())
            font = io.Fonts->AddFontFromFileTTF(path.c_str(), size);
        return merge(font, size);
    };
    fonts.body = load(L"FONT_BODY", L"segoeui.ttf", 15);
    if (!fonts.body) {
        fonts.body = io.Fonts->AddFontDefault();
        fonts.icons = false;
    }
    fonts.semibold = load(L"FONT_SEMIBOLD", L"seguisb.ttf", 15);
    fonts.label = load(L"FONT_LABEL", L"segoeuib.ttf", 15);
    fonts.kicker = load(L"FONT_KICKER", L"seguisb.ttf", 13);
    fonts.display = load(L"FONT_DISPLAY", L"georgiab.ttf", 34);
    fonts.narration = load(nullptr, L"georgiai.ttf", 16);
    if (!fonts.narration)
        fonts.narration = load(nullptr, L"segoeuii.ttf", 16);
    if (const std::string mono = file(L"consola.ttf"); !mono.empty())
        fonts.mono = io.Fonts->AddFontFromFileTTF(mono.c_str(), 13);
    if (!fonts.semibold) fonts.semibold = fonts.body;
    if (!fonts.label) fonts.label = fonts.semibold;
    if (!fonts.kicker) fonts.kicker = fonts.label;
    if (!fonts.display) fonts.display = fonts.label;
    if (!fonts.narration) fonts.narration = fonts.body;
    if (!fonts.mono) fonts.mono = fonts.body;
    return fonts;
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, w, l))
        return TRUE;
    switch (message) {
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) {
            resizeWidth = LOWORD(l);
            resizeHeight = HIWORD(l);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(l);
        info->ptMinTrackSize = {static_cast<LONG>(1000 * dpiScale), static_cast<LONG>(720 * dpiScale)};
        return 0;
    }
    case WM_DPICHANGED: {
        dpiScale = HIWORD(w) / 96.f;
        dpiChanged = true;
        const auto* suggested = reinterpret_cast<RECT*>(l);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((w & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int count = 0;
    std::string updatedFrom; // --updated-from: the version an update just replaced (the banner says so)
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 1; args && i < count; ++i) {
        if (wcscmp(args[i], L"--updated-from") == 0 && i + 1 < count)
            updatedFrom = launcher::narrow(args[i + 1]);
        // The headset check: no window, its answer on the output the launcher reads.
        if (wcscmp(args[i], L"--probe") == 0) {
            LocalFree(args);
            return launcher::runProbe();
        }
        // Started again as administrator (restartAsAdministrator) or after an update (restartLauncher): wait for the
        // one that started this one to close.
        if (wcscmp(args[i], L"--wait-for") == 0 && i + 1 < count) {
            if (HANDLE previous = OpenProcess(SYNCHRONIZE, FALSE, wcstoul(args[i + 1], nullptr, 10))) {
                WaitForSingleObject(previous, 20000);
                CloseHandle(previous);
            }
        }
    }
    if (args)
        LocalFree(args);
    // One launcher at a time: a second start brings the first one forward. A first one that runs as
    // administrator keeps its lock closed to a second that does not (access denied, no handle).
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\ERVRLauncher");
    if (const DWORD result = GetLastError();
        result == ERROR_ALREADY_EXISTS || (!single && result == ERROR_ACCESS_DENIED)) {
        if (HWND other = FindWindowW(kClass, nullptr)) {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ImGui_ImplWin32_EnableDpiAwareness();
    const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    dpiScale = ImGui_ImplWin32_GetDpiScaleForMonitor(monitor);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    const int areaWidth = info.rcWork.right - info.rcWork.left, areaHeight = info.rcWork.bottom - info.rcWork.top;
    const int width = std::min(static_cast<int>(1160 * dpiScale), areaWidth * 94 / 100);
    const int height = std::min(static_cast<int>(880 * dpiScale), areaHeight * 94 / 100);

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = CreateSolidBrush(kGround);
    windowClass.lpszClassName = kClass;
    RegisterClassExW(&windowClass);
    HWND window = CreateWindowExW(0, kClass, L"ERVR - ELDEN RING VR", WS_OVERLAPPEDWINDOW,
                                  info.rcWork.left + (areaWidth - width) / 2, info.rcWork.top + (areaHeight - height) / 2,
                                  width, height, nullptr, nullptr, instance, nullptr);
    // A dark title bar in the window's own colour (Windows 11 takes the colour; 10 only the dark mode).
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    const COLORREF caption = kGround;
    DwmSetWindowAttribute(window, 35, &caption, sizeof(caption));
    if (!createDevice(window)) {
        releaseDevice();
        MessageBoxW(nullptr, L"The ERVR launcher needs DirectX 11 graphics, which this PC did not provide.", L"ERVR",
                    MB_ICONERROR);
        return 1;
    }
    createTarget();
    ShowWindow(window, show);
    UpdateWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    const launcher::Fonts fonts = loadFonts();
    applyStyle(dpiScale);
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(device, context);
    std::wstring restartFolder; // an update installed itself: its launcher starts once this one has let go of all
    {
        launcher::App application(window, fonts, device, updatedFrom);
        bool done = false, occluded = false;
        while (!done) {
            // In the background (the game in front) a few frames a second are plenty.
            const bool foreground = GetForegroundWindow() == window;
            if (!foreground || IsIconic(window) || occluded)
                MsgWaitForMultipleObjects(0, nullptr, FALSE, application.busy() ? 100 : 400, QS_ALLINPUT);
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
                if (message.message == WM_QUIT)
                    done = true;
            }
            if (done)
                break;
            application.poll();
            if (application.shouldQuit()) {
                restartFolder = application.restartFolder();
                DestroyWindow(window);
                continue;
            }
            if (IsIconic(window))
                continue;
            if (occluded && swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
                continue;
            occluded = false;
            if (dpiChanged) {
                applyStyle(dpiScale);
                dpiChanged = false;
            }
            if (resizeWidth && resizeHeight) {
                releaseTarget();
                swapChain->ResizeBuffers(0, resizeWidth, resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
                resizeWidth = resizeHeight = 0;
                createTarget();
            }
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            application.frame(dpiScale);
            ImGui::Render();
            const float clear[4] = {(kGround & 0xff) / 255.f, ((kGround >> 8) & 0xff) / 255.f,
                                    ((kGround >> 16) & 0xff) / 255.f, 1};
            context->OMSetRenderTargets(1, &target, nullptr);
            context->ClearRenderTargetView(target, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            occluded = swapChain->Present(foreground ? 1 : 0, 0) == DXGI_STATUS_OCCLUDED;
        }
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    releaseDevice();
    CoUninitialize();
    if (single)
        CloseHandle(single);
    // Only now, with the window, ImGui and the one-launcher lock gone, can the updated launcher start and own them.
    if (!restartFolder.empty()) {
        std::string error;
        if (!launcher::restartLauncher(restartFolder, error))
            MessageBoxW(nullptr, launcher::widen(error).c_str(), L"ERVR", MB_OK | MB_ICONWARNING);
    }
    return 0;
}
