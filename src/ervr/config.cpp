#include "config.h"
#include "log.h"
#include <windows.h>
#include <xinput.h>
#include <cctype>

namespace ervr {

namespace {
Config g_cfg;

std::string Upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }

std::string GetS(const wchar_t* ini, const wchar_t* sec, const wchar_t* key, const std::string& def) {
    wchar_t buf[512]{};
    std::wstring wdef(def.begin(), def.end());
    GetPrivateProfileStringW(sec, key, wdef.c_str(), buf, 512, ini);
    std::string out;
    for (wchar_t* p = buf; *p; ++p) out.push_back((char)*p);
    while (!out.empty() && isspace((unsigned char)out.back())) out.pop_back();
    size_t i = 0; while (i < out.size() && isspace((unsigned char)out[i])) ++i;
    return out.substr(i);
}
float GetF(const wchar_t* ini, const wchar_t* sec, const wchar_t* key, float def) {
    std::string s = GetS(ini, sec, key, "");
    if (s.empty()) return def;
    try { return std::stof(s); } catch (...) { return def; }
}
int GetI(const wchar_t* ini, const wchar_t* sec, const wchar_t* key, int def) {
    std::string s = GetS(ini, sec, key, "");
    if (s.empty()) return def;
    try { return (int)std::stol(s, nullptr, 0); } catch (...) { return def; }
}
bool GetB(const wchar_t* ini, const wchar_t* sec, const wchar_t* key, bool def) {
    std::string s = Upper(GetS(ini, sec, key, ""));
    if (s.empty()) return def;
    return s == "1" || s == "TRUE" || s == "YES" || s == "ON";
}
} // namespace

uint16_t ParseXInputButton(const std::string& raw) {
    std::string n = Upper(raw);
    if (n == "A") return XINPUT_GAMEPAD_A;
    if (n == "B") return XINPUT_GAMEPAD_B;
    if (n == "X") return XINPUT_GAMEPAD_X;
    if (n == "Y") return XINPUT_GAMEPAD_Y;
    if (n == "LB" || n == "L1") return XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (n == "RB" || n == "R1") return XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (n == "LS" || n == "L3") return XINPUT_GAMEPAD_LEFT_THUMB;
    if (n == "RS" || n == "R3") return XINPUT_GAMEPAD_RIGHT_THUMB;
    if (n == "START" || n == "MENU") return XINPUT_GAMEPAD_START;
    if (n == "BACK" || n == "SELECT" || n == "VIEW") return XINPUT_GAMEPAD_BACK;
    if (n == "DPAD_UP" || n == "UP") return XINPUT_GAMEPAD_DPAD_UP;
    if (n == "DPAD_DOWN" || n == "DOWN") return XINPUT_GAMEPAD_DPAD_DOWN;
    if (n == "DPAD_LEFT" || n == "LEFT") return XINPUT_GAMEPAD_DPAD_LEFT;
    if (n == "DPAD_RIGHT" || n == "RIGHT") return XINPUT_GAMEPAD_DPAD_RIGHT;
    if (n == "LT" || n == "L2") return VP_LT;
    if (n == "RT" || n == "R2") return VP_RT;
    return 0;
}

const char* XInputButtonName(uint16_t m) {
    switch (m) {
    case XINPUT_GAMEPAD_A: return "A"; case XINPUT_GAMEPAD_B: return "B";
    case XINPUT_GAMEPAD_X: return "X"; case XINPUT_GAMEPAD_Y: return "Y";
    case XINPUT_GAMEPAD_LEFT_SHOULDER: return "LB"; case XINPUT_GAMEPAD_RIGHT_SHOULDER: return "RB";
    case XINPUT_GAMEPAD_LEFT_THUMB: return "LS"; case XINPUT_GAMEPAD_RIGHT_THUMB: return "RS";
    case XINPUT_GAMEPAD_START: return "START"; case XINPUT_GAMEPAD_BACK: return "BACK";
    case XINPUT_GAMEPAD_DPAD_UP: return "DPAD_UP"; case XINPUT_GAMEPAD_DPAD_DOWN: return "DPAD_DOWN";
    case XINPUT_GAMEPAD_DPAD_LEFT: return "DPAD_LEFT"; case XINPUT_GAMEPAD_DPAD_RIGHT: return "DPAD_RIGHT";
    case VP_LT: return "LT"; case VP_RT: return "RT";
    default: return "none";
    }
}

void Config::Load(const std::wstring& path) {
    const wchar_t* ini = path.c_str();
    logLevel   = GetS(ini, L"General", L"LogLevel", logLevel);
    vrRuntime  = GetS(ini, L"General", L"VRRuntime", vrRuntime);
    enabled    = GetB(ini, L"General", L"Enabled", enabled);
    pollHz     = GetI(ini, L"General", L"PollHz", pollHz);

    btnA = GetS(ini, L"Buttons", L"A", btnA); btnB = GetS(ini, L"Buttons", L"B", btnB);
    btnX = GetS(ini, L"Buttons", L"X", btnX); btnY = GetS(ini, L"Buttons", L"Y", btnY);
    btnLThumb = GetS(ini, L"Buttons", L"LeftStickClick", btnLThumb);
    btnRThumb = GetS(ini, L"Buttons", L"RightStickClick", btnRThumb);
    btnMenu   = GetS(ini, L"Buttons", L"Menu", btnMenu);
    btnLTrigger = GetS(ini, L"Buttons", L"LeftTrigger", btnLTrigger);
    btnRTrigger = GetS(ini, L"Buttons", L"RightTrigger", btnRTrigger);
    btnLGrip  = GetS(ini, L"Buttons", L"LeftGrip", btnLGrip);
    btnRGrip  = GetS(ini, L"Buttons", L"RightGrip", btnRGrip);
    triggerThreshold = GetF(ini, L"Buttons", L"TriggerThreshold", triggerThreshold);
    gripThreshold    = GetF(ini, L"Buttons", L"GripThreshold", gripThreshold);
    menuHoldForBack  = GetF(ini, L"Buttons", L"MenuHoldForBack", menuHoldForBack);
    leftGripPlusStickIsDpad = GetB(ini, L"Buttons", L"LeftGripPlusStickIsDpad", leftGripPlusStickIsDpad);

    stickDeadzone = GetF(ini, L"Sticks", L"Deadzone", stickDeadzone);
    swapSticks    = GetB(ini, L"Sticks", L"Swap", swapSticks);
    rightStickSensitivity = GetF(ini, L"Sticks", L"RightSensitivity", rightStickSensitivity);

    gesturesEnabled  = GetB(ini, L"Gestures", L"Enabled", gesturesEnabled);
    lightAttackSpeed = GetF(ini, L"Gestures", L"LightAttackSpeed", lightAttackSpeed);
    heavyAttackSpeed = GetF(ini, L"Gestures", L"HeavyAttackSpeed", heavyAttackSpeed);
    leftSwingSpeed   = GetF(ini, L"Gestures", L"LeftSwingSpeed", leftSwingSpeed);
    lightAttackButton = GetS(ini, L"Gestures", L"LightAttackButton", lightAttackButton);
    heavyAttackButton = GetS(ini, L"Gestures", L"HeavyAttackButton", heavyAttackButton);
    leftSwingButton   = GetS(ini, L"Gestures", L"LeftSwingButton", leftSwingButton);
    gestureCooldown   = GetF(ini, L"Gestures", L"Cooldown", gestureCooldown);
    gesturePressDuration = GetF(ini, L"Gestures", L"PressDuration", gesturePressDuration);
    gestureRequiresGrip  = GetB(ini, L"Gestures", L"RequireGrip", gestureRequiresGrip);
    gestureHaptics       = GetB(ini, L"Gestures", L"Haptics", gestureHaptics);

    handRelativeMovement = GetB(ini, L"Movement", L"HandRelative", handRelativeMovement);
    snapTurn        = GetB(ini, L"Movement", L"SnapTurn", snapTurn);
    snapTurnPulseSeconds  = GetF(ini, L"Movement", L"SnapTurnPulseSeconds", snapTurnPulseSeconds);
    snapTurnRepeatSeconds = GetF(ini, L"Movement", L"SnapTurnRepeatSeconds", snapTurnRepeatSeconds);

    rumbleToControllers = GetB(ini, L"Haptics", L"RumbleToControllers", rumbleToControllers);
    rumbleScale = GetF(ini, L"Haptics", L"RumbleScale", rumbleScale);

    fpEnabled = GetB(ini, L"FirstPerson", L"Enabled", fpEnabled);
    fpEyeHeight = GetF(ini, L"FirstPerson", L"EyeHeight", fpEyeHeight);
    fpCameraMode = GetS(ini, L"FirstPerson", L"CameraMode", fpCameraMode);
    fpCameraRotation = GetS(ini, L"FirstPerson", L"CameraRotation", fpCameraRotation);
    fpAddHmdPosition = GetB(ini, L"FirstPerson", L"AddHmdPosition", fpAddHmdPosition);
    fpHmdYawInGameCamera = GetB(ini, L"FirstPerson", L"HmdYawInGameCamera", fpHmdYawInGameCamera);
    fpFaceCamera = GetB(ini, L"FirstPerson", L"FaceCamera", fpFaceCamera);
    fpHideHead = GetB(ini, L"FirstPerson", L"HideHead", fpHideHead);
    fpHideSheathed = GetB(ini, L"FirstPerson", L"HideSheathedWeapons", fpHideSheathed);
    fpWriteAim = GetB(ini, L"FirstPerson", L"WriteAim", fpWriteAim);
    handTracking = GetB(ini, L"FirstPerson", L"HandTracking", handTracking);
    handOrigin = GetS(ini, L"FirstPerson", L"HandOrigin", handOrigin);
    handBackOffset = GetF(ini, L"FirstPerson", L"HandBackOffset", handBackOffset);
    handRotOffX = GetF(ini, L"FirstPerson", L"HandRotOffsetX", handRotOffX);
    handRotOffY = GetF(ini, L"FirstPerson", L"HandRotOffsetY", handRotOffY);
    handRotOffZ = GetF(ini, L"FirstPerson", L"HandRotOffsetZ", handRotOffZ);
    elbowPoleDown = GetF(ini, L"FirstPerson", L"ElbowPoleDown", elbowPoleDown);
    elbowPoleOut = GetF(ini, L"FirstPerson", L"ElbowPoleOut", elbowPoleOut);
    elbowPoleBack = GetF(ini, L"FirstPerson", L"ElbowPoleBack", elbowPoleBack);
    armStretch = GetF(ini, L"FirstPerson", L"ArmStretch", armStretch);
    handScale = GetF(ini, L"FirstPerson", L"HandScale", handScale);
    torsoFaceCamera = GetB(ini, L"FirstPerson", L"TorsoFaceCamera", torsoFaceCamera);
    boneTorso = GetS(ini, L"FirstPerson", L"BoneTorso", boneTorso);
    boneLUpperArm = GetS(ini, L"FirstPerson", L"BoneLeftUpperArm", boneLUpperArm);
    boneLForearm = GetS(ini, L"FirstPerson", L"BoneLeftForearm", boneLForearm);
    boneLHand = GetS(ini, L"FirstPerson", L"BoneLeftHand", boneLHand);
    boneRUpperArm = GetS(ini, L"FirstPerson", L"BoneRightUpperArm", boneRUpperArm);
    boneRForearm = GetS(ini, L"FirstPerson", L"BoneRightForearm", boneRForearm);
    boneRHand = GetS(ini, L"FirstPerson", L"BoneRightHand", boneRHand);
    dumpSkeleton = GetB(ini, L"FirstPerson", L"DumpSkeleton", dumpSkeleton);
    fpDebug = GetB(ini, L"FirstPerson", L"Debug", fpDebug);

    menuNavEnabled = GetB(ini, L"MenuNav", L"Enabled", menuNavEnabled);
    menuNavThreshold = GetF(ini, L"MenuNav", L"Threshold", menuNavThreshold);
    menuNavFollowStart = GetB(ini, L"MenuNav", L"FollowStartButton", menuNavFollowStart);
    menuNavToggleHold = GetF(ini, L"MenuNav", L"ToggleHold", menuNavToggleHold);
    menuNavHaptics = GetB(ini, L"MenuNav", L"Haptics", menuNavHaptics);
    menuAob = GetS(ini, L"MenuNav", L"MenuAOB", menuAob);
    menuAobOffset = GetI(ini, L"MenuNav", L"MenuAOBOffset", menuAobOffset);
    menuAobInstrLen = GetI(ini, L"MenuNav", L"MenuAOBInstrLen", menuAobInstrLen);
    menuChain = GetS(ini, L"MenuNav", L"MenuChain", menuChain);
    menuScan = GetB(ini, L"MenuNav", L"Scan", menuScan);
    menuScanSize = GetI(ini, L"MenuNav", L"ScanSize", menuScanSize);

    handAimEnabled = GetB(ini, L"Experimental", L"HandAim", handAimEnabled);
    cameraAob      = GetS(ini, L"Experimental", L"CameraAOB", cameraAob);
    cameraAobOffset = GetI(ini, L"Experimental", L"CameraAOBOffset", cameraAobOffset);
    cameraAobInstrLen = GetI(ini, L"Experimental", L"CameraAOBInstrLen", cameraAobInstrLen);
    cameraChain    = GetS(ini, L"Experimental", L"CameraChain", cameraChain);
    cameraYawOffset   = GetI(ini, L"Experimental", L"CameraYawOffset", cameraYawOffset);
    cameraPitchOffset = GetI(ini, L"Experimental", L"CameraPitchOffset", cameraPitchOffset);
    handAimStrength = GetF(ini, L"Experimental", L"HandAimStrength", handAimStrength);
}

Config& Cfg() { return g_cfg; }
}
