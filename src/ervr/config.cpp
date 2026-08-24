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
    this->iniPath = path;
    const wchar_t* ini = path.c_str();
    logLevel   = GetS(ini, L"General", L"LogLevel", logLevel);
    vrRuntime  = GetS(ini, L"General", L"VRRuntime", vrRuntime);
    enabled    = GetB(ini, L"General", L"Enabled", enabled);
    pollHz     = GetI(ini, L"General", L"PollHz", pollHz);

    vrStereoMode     = GetS(ini, L"VR", L"StereoMode", vrStereoMode);
    vrRenderScale    = GetF(ini, L"VR", L"RenderScale", vrRenderScale);
    vrWorldScale     = GetF(ini, L"VR", L"WorldScale", vrWorldScale);
    vrPairRateHz     = GetI(ini, L"VR", L"PairRateHz", vrPairRateHz);
    vrCinemaDistance = GetF(ini, L"VR", L"CinemaDistance", vrCinemaDistance);
    vrCinemaWidth    = GetF(ini, L"VR", L"CinemaWidth", vrCinemaWidth);
    vrMirrorWindow   = GetB(ini, L"VR", L"MirrorWindow", vrMirrorWindow);
    vrXrDebug        = GetB(ini, L"VR", L"XrDebug", vrXrDebug);
    vrFovDeg         = GetF(ini, L"VR", L"FovDeg", vrFovDeg);
    vrFovAxis        = GetS(ini, L"VR", L"FovAxis", vrFovAxis);
    vrGameRasterRefHeight = GetF(ini, L"VR", L"GameRasterRefHeight", vrGameRasterRefHeight);
    vrIpdMm          = GetF(ini, L"VR", L"IpdMm", vrIpdMm);
    vrColorFix       = GetS(ini, L"VR", L"ColorFix", vrColorFix);
    vrGameRes        = GetS(ini, L"VR", L"GameRes", vrGameRes);
    vrGameResScale   = GetF(ini, L"VR", L"GameResScale", vrGameResScale);
    vrGameResOversize = GetB(ini, L"VR", L"GameResOversize", vrGameResOversize);
    vrGameResScan    = GetB(ini, L"VR", L"GameResScan", vrGameResScan);
    vrGameResScanSeconds = GetF(ini, L"VR", L"GameResScanSeconds", vrGameResScanSeconds);
    vrGameResAspectFix = GetB(ini, L"VR", L"GameResAspectFix", vrGameResAspectFix);
    vrPass2Keep      = GetS(ini, L"VR", L"Pass2Keep", vrPass2Keep);
    vrPass2FullFrame = GetB(ini, L"VR", L"Pass2FullFrame", vrPass2FullFrame);
    vrPass2Dt        = GetF(ini, L"VR", L"Pass2Dt", vrPass2Dt);
    vrPass2FullFrameKeep = GetS(ini, L"VR", L"Pass2FullFrameKeep", vrPass2FullFrameKeep);
    vrPass2SafeKeep = GetS(ini, L"VR", L"Pass2SafeKeep", vrPass2SafeKeep);
    vrPass2SafeSeconds = GetF(ini, L"VR", L"Pass2SafeSeconds", vrPass2SafeSeconds);
    vrPass2SpawnGraceSeconds = GetF(ini, L"VR", L"Pass2SpawnGraceSeconds", vrPass2SpawnGraceSeconds);
    vrPass2MenuGraceSeconds = GetF(ini, L"VR", L"Pass2MenuGraceSeconds", vrPass2MenuGraceSeconds);
    vrPass2WarpGraceSeconds = GetF(ini, L"VR", L"Pass2WarpGraceSeconds", vrPass2WarpGraceSeconds);
    vrPass2FinalizeReplay = GetB(ini, L"VR", L"Pass2FinalizeReplay", vrPass2FinalizeReplay);
    vrPass2SafeOnA = GetF(ini, L"VR", L"Pass2SafeOnA", vrPass2SafeOnA);
    vrReportFinalCamera = GetB(ini, L"VR", L"ReportFinalCamera", vrReportFinalCamera);
    vrPass2Steps     = GetI(ini, L"VR", L"Pass2Steps", vrPass2Steps);
    vrPass2EatAsserts = GetB(ini, L"VR", L"Pass2EatAsserts", vrPass2EatAsserts);
    vrFadeGuard      = GetB(ini, L"VR", L"FadeGuard", vrFadeGuard);
    vrPresentLag     = GetI(ini, L"VR", L"PresentLag", vrPresentLag);
    vrGpuSync        = GetI(ini, L"VR", L"GpuSync", vrGpuSync);
    debugKeys        = GetB(ini, L"VR", L"DebugKeys", debugKeys);
    vrDebugPass2Pitch = GetF(ini, L"VR", L"DebugPass2Pitch", vrDebugPass2Pitch);

    hudMode         = GetS(ini, L"HUD", L"Mode", hudMode);
    hudTarget       = GetS(ini, L"HUD", L"Target", hudTarget);
    hudFirstDraw    = GetI(ini, L"HUD", L"FirstDraw", hudFirstDraw);
    hudProbe        = GetB(ini, L"HUD", L"Probe", hudProbe);
    hudLock         = GetS(ini, L"HUD", L"Lock", hudLock);
    hudDistance     = GetF(ini, L"HUD", L"Distance", hudDistance);
    hudWidth        = GetF(ini, L"HUD", L"Width", hudWidth);
    hudHeightOffset = GetF(ini, L"HUD", L"HeightOffset", hudHeightOffset);
    hudYawDeg       = GetF(ini, L"HUD", L"YawDeg", hudYawDeg);
    hudPitchDeg     = GetF(ini, L"HUD", L"PitchDeg", hudPitchDeg);
    hudOpacity      = GetF(ini, L"HUD", L"Opacity", hudOpacity);
    hudKey          = GetS(ini, L"HUD", L"Key", hudKey);
    hudRedirectWithoutVR = GetB(ini, L"HUD", L"RedirectWithoutVR", hudRedirectWithoutVR);
    hudMirrorPanel  = GetB(ini, L"HUD", L"MirrorPanel", hudMirrorPanel);
    hudFollowDeg    = GetF(ini, L"HUD", L"FollowDeg", hudFollowDeg);
    hudFollowRadius = GetF(ini, L"HUD", L"FollowRadius", hudFollowRadius);

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
    moveForward = GetS(ini, L"Movement", L"Forward", handRelativeMovement ? std::string("hand") : moveForward);
    rightStick = GetS(ini, L"Movement", L"RightStick", rightStick);
    cameraTurnErvr  = GetB(ini, L"Movement", L"CameraTurnByErvr", cameraTurnErvr);
    turnSpeedDegPerSec = GetF(ini, L"Movement", L"TurnSpeed", turnSpeedDegPerSec);
    snapTurnDegrees = GetF(ini, L"Movement", L"SnapTurnDegrees", snapTurnDegrees);
    moveRelativeToCamera = GetB(ini, L"Movement", L"RelativeToCamera", moveRelativeToCamera);
    snapTurn        = GetB(ini, L"Movement", L"SnapTurn", snapTurn);
    snapTurnPulseSeconds  = GetF(ini, L"Movement", L"SnapTurnPulseSeconds", snapTurnPulseSeconds);
    snapTurnRepeatSeconds = GetF(ini, L"Movement", L"SnapTurnRepeatSeconds", snapTurnRepeatSeconds);

    rumbleToControllers = GetB(ini, L"Haptics", L"RumbleToControllers", rumbleToControllers);
    rumbleScale = GetF(ini, L"Haptics", L"RumbleScale", rumbleScale);

    combatEnabled = GetB(ini, L"Combat", L"Enabled", combatEnabled);
    combatMode = GetS(ini, L"Combat", L"Mode", combatMode);
    swingOnSpeed = GetF(ini, L"Combat", L"SwingOnSpeed", swingOnSpeed);
    swingOffSpeed = GetF(ini, L"Combat", L"SwingOffSpeed", swingOffSpeed);
    heavySwingSpeed = GetF(ini, L"Combat", L"HeavySwingSpeed", heavySwingSpeed);
    minHandSpeed = GetF(ini, L"Combat", L"MinHandSpeed", minHandSpeed);
    weaponReachRight = GetF(ini, L"Combat", L"WeaponReachRight", weaponReachRight);
    weaponReachLeft = GetF(ini, L"Combat", L"WeaponReachLeft", weaponReachLeft);
    swingArmWindow = GetF(ini, L"Combat", L"ArmWindow", swingArmWindow);
    swingMaxSeconds = GetF(ini, L"Combat", L"MaxSwingSeconds", swingMaxSeconds);
    swingCooldown = GetF(ini, L"Combat", L"Cooldown", swingCooldown);
    combatLeftHand = GetB(ini, L"Combat", L"LeftHand", combatLeftHand);
    combatRequireStamina = GetB(ini, L"Combat", L"RequireStamina", combatRequireStamina);
    combatMinStamina = GetI(ini, L"Combat", L"MinStamina", combatMinStamina);
    lightJudgeId = GetI(ini, L"Combat", L"LightJudgeId", lightJudgeId);
    heavyJudgeId = GetI(ini, L"Combat", L"HeavyJudgeId", heavyJudgeId);
    leftJudgeId = GetI(ini, L"Combat", L"LeftJudgeId", leftJudgeId);
    combatLearn = GetB(ini, L"Combat", L"Learn", combatLearn);
    combatSaveLearned = GetB(ini, L"Combat", L"SaveLearned", combatSaveLearned);
    combatBlockAttackButtons = GetB(ini, L"Combat", L"BlockAttackButtons", combatBlockAttackButtons);
    combatAttackIndexRotate = GetI(ini, L"Combat", L"AttackIndexRotate", combatAttackIndexRotate);
    combatExtraStamina[0] = GetI(ini, L"Combat", L"ExtraStaminaLight", combatExtraStamina[0]);
    combatExtraStamina[1] = GetI(ini, L"Combat", L"ExtraStaminaHeavy", combatExtraStamina[1]);
    combatExtraStamina[2] = GetI(ini, L"Combat", L"ExtraStaminaLeft", combatExtraStamina[2]);
    combatSuppressAnimAttacks = GetB(ini, L"Combat", L"SuppressAnimAttacks", combatSuppressAnimAttacks);
    cheatSetAllStats = GetI(ini, L"Cheats", L"SetAllStats", cheatSetAllStats);
    combatDebug = GetB(ini, L"Combat", L"Debug", combatDebug);
    kineticEnabled = GetB(ini, L"Combat", L"Kinetic", kineticEnabled);
    kineticRefSpeed = GetF(ini, L"Combat", L"KineticRefSpeed", kineticRefSpeed);
    kineticArcRef = GetF(ini, L"Combat", L"KineticArc", kineticArcRef);
    kineticMinMult = GetF(ini, L"Combat", L"KineticMinMult", kineticMinMult);
    kineticMaxMult = GetF(ini, L"Combat", L"KineticMaxMult", kineticMaxMult);
    kineticHeavyQuality = GetF(ini, L"Combat", L"KineticHeavyQuality", kineticHeavyQuality);
    kineticAccel = GetF(ini, L"Combat", L"KineticAccel", kineticAccel);
    kineticAccelWeight = GetF(ini, L"Combat", L"KineticAccelWeight", kineticAccelWeight);
    kineticInertia = GetB(ini, L"Combat", L"Inertia", kineticInertia);
    inertiaAngAccel = GetF(ini, L"Combat", L"InertiaAngAccel", inertiaAngAccel);
    inertiaSpringHz = GetF(ini, L"Combat", L"InertiaSpringHz", inertiaSpringHz);
    kineticArmPerWeight = GetF(ini, L"Combat", L"KineticArmPerWeight", kineticArmPerWeight);
    kineticStaminaScale = GetF(ini, L"Combat", L"KineticStaminaScale", kineticStaminaScale);
    kineticStaminaPerStrike = GetF(ini, L"Combat", L"KineticStaminaPerStrike", kineticStaminaPerStrike);
    kineticFatigueStart = GetF(ini, L"Combat", L"KineticFatigueStart", kineticFatigueStart);
    kineticFatigueFloor = GetF(ini, L"Combat", L"KineticFatigueFloor", kineticFatigueFloor);
    kineticPoiseScales = GetB(ini, L"Combat", L"KineticPoiseScales", kineticPoiseScales);
    kineticStaminaDamageScales = GetB(ini, L"Combat", L"KineticStaminaDamageScales", kineticStaminaDamageScales);
    blockEnabled = GetB(ini, L"Combat", L"Block", blockEnabled);
    guardButton = GetS(ini, L"Combat", L"GuardButton", guardButton);
    shieldWepTypes = GetS(ini, L"Combat", L"ShieldWepTypes", shieldWepTypes);
    blockReleaseSeconds = GetF(ini, L"Combat", L"BlockReleaseSeconds", blockReleaseSeconds);
    blockMinCut = GetF(ini, L"Combat", L"BlockMinCut", blockMinCut);
    blockMinStability = GetF(ini, L"Combat", L"BlockMinStability", blockMinStability);
    shieldNormalPitchDeg = GetF(ini, L"Combat", L"ShieldNormalPitch", shieldNormalPitchDeg);
    deflectSpeed = GetF(ini, L"Combat", L"DeflectSpeed", deflectSpeed);
    blockImpactStamina = GetI(ini, L"Combat", L"BlockImpactStamina", blockImpactStamina);

    fpEnabled = GetB(ini, L"FirstPerson", L"Enabled", fpEnabled);
    fpEyeHeight = GetF(ini, L"FirstPerson", L"EyeHeight", fpEyeHeight);
    fpCameraAnchor = GetS(ini, L"FirstPerson", L"CameraAnchor", fpCameraAnchor);
    fpCameraAnchorSmoothing = GetF(ini, L"FirstPerson", L"CameraAnchorSmoothing", fpCameraAnchorSmoothing);
    fpCameraBob = GetF(ini, L"FirstPerson", L"CameraBob", fpCameraBob);
    locomotion = GetS(ini, L"Locomotion", L"Mode", locomotion);
    locoRunSpeed = GetF(ini, L"Locomotion", L"RunSpeed", locoRunSpeed);
    locoSprintSpeed = GetF(ini, L"Locomotion", L"SprintSpeed", locoSprintSpeed);
    locoFeedStick = GetB(ini, L"Locomotion", L"FeedStick", locoFeedStick);
    fpCameraMode = GetS(ini, L"FirstPerson", L"CameraMode", fpCameraMode);
    fpCameraRotation = GetS(ini, L"FirstPerson", L"CameraRotation", fpCameraRotation);
    fpCameraYawSource = GetS(ini, L"FirstPerson", L"CameraYawSource", fpCameraYawSource);
    fpOwnChrCam = GetB(ini, L"FirstPerson", L"OwnChrCam", fpOwnChrCam);
    fpRecenter = GetB(ini, L"FirstPerson", L"Recenter", fpRecenter);
    fpRecenterSeconds = GetF(ini, L"FirstPerson", L"RecenterSeconds", fpRecenterSeconds);
    fpRecenterMaxRadius = GetF(ini, L"FirstPerson", L"RecenterMaxRadius", fpRecenterMaxRadius);
    fpRecenterSnapRadius = GetF(ini, L"FirstPerson", L"RecenterSnapRadius", fpRecenterSnapRadius);
    fpRecenterHeight = GetB(ini, L"FirstPerson", L"RecenterHeight", fpRecenterHeight);
    fpFaceCamera = GetB(ini, L"FirstPerson", L"FaceCamera", fpFaceCamera);
    fpPoseMode = GetS(ini, L"FirstPerson", L"PoseMode", fpPoseMode);
    {   // "auto" resolves against PoseMode on every (re)load so hot-editing PoseMode keeps them consistent
        std::string fcm = GetS(ini, L"FirstPerson", L"FaceCameraMode", "auto");
        fpFaceCameraMode = (fcm == "auto") ? (fpPoseMode == "vr" ? std::string("strafe") : std::string("lock")) : fcm;
    }
    fpLockTargetDistance = GetF(ini, L"FirstPerson", L"LockTargetDistance", fpLockTargetDistance);
    fpArmRootSource = GetS(ini, L"FirstPerson", L"ArmRootSource", fpArmRootSource);
    fpPinBodyFacing = GetB(ini, L"FirstPerson", L"PinBodyFacing", fpPinBodyFacing);
    fpPoseAfterHavok = GetB(ini, L"FirstPerson", L"PoseAfterHavok", fpPoseAfterHavok);
    fpLatchVRPerFrame = GetB(ini, L"FirstPerson", L"LatchVRPerFrame", fpLatchVRPerFrame);
    fpCameraFinalWrite = GetB(ini, L"FirstPerson", L"CameraFinalWrite", fpCameraFinalWrite);
    fpCameraAfterModelPos = GetB(ini, L"FirstPerson", L"CameraAfterModelPos", fpCameraAfterModelPos);
    fpArmSolveEveryCall = GetB(ini, L"FirstPerson", L"ArmSolveEveryCall", fpArmSolveEveryCall);
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
    bodyYawSource = GetS(ini, L"FirstPerson", L"BodyYaw", bodyYawSource);
    bodyFacingOffsetDeg = GetF(ini, L"FirstPerson", L"BodyFacingOffsetDeg", bodyFacingOffsetDeg);
    bodyYawFollowSeconds = GetF(ini, L"FirstPerson", L"BodyYawFollowSeconds", bodyYawFollowSeconds);
    bodyYawDeadzoneDeg = GetF(ini, L"FirstPerson", L"BodyYawDeadzoneDeg", bodyYawDeadzoneDeg);
    torsoFaceCamera = GetB(ini, L"FirstPerson", L"TorsoFaceCamera", torsoFaceCamera);
    bodyFollowsHead = GetB(ini, L"FirstPerson", L"BodyFollowsHead", bodyFollowsHead);
    bodyAnchor = GetS(ini, L"FirstPerson", L"BodyAnchor", bodyAnchor);
    bodyRotateToHead = GetB(ini, L"FirstPerson", L"BodyRotate", bodyRotateToHead);
    bodyFollowMaxHorizontal = GetF(ini, L"FirstPerson", L"BodyFollowMaxHorizontal", bodyFollowMaxHorizontal);
    bodyFollowMaxDown = GetF(ini, L"FirstPerson", L"BodyFollowMaxDown", bodyFollowMaxDown);
    bodyFollowMaxUp = GetF(ini, L"FirstPerson", L"BodyFollowMaxUp", bodyFollowMaxUp);
    boneTorso = GetS(ini, L"FirstPerson", L"BoneTorso", boneTorso);
    boneHead = GetS(ini, L"FirstPerson", L"BoneHead", boneHead);
    boneLUpperArm = GetS(ini, L"FirstPerson", L"BoneLeftUpperArm", boneLUpperArm);
    boneLForearm = GetS(ini, L"FirstPerson", L"BoneLeftForearm", boneLForearm);
    boneLHand = GetS(ini, L"FirstPerson", L"BoneLeftHand", boneLHand);
    boneRUpperArm = GetS(ini, L"FirstPerson", L"BoneRightUpperArm", boneRUpperArm);
    boneRForearm = GetS(ini, L"FirstPerson", L"BoneRightForearm", boneRForearm);
    boneRHand = GetS(ini, L"FirstPerson", L"BoneRightHand", boneRHand);
    dumpSkeleton = GetB(ini, L"FirstPerson", L"DumpSkeleton", dumpSkeleton);
    fpPoseScan = GetB(ini, L"FirstPerson", L"PoseWriterScan", fpPoseScan);
    fpDebug = GetB(ini, L"FirstPerson", L"Debug", fpDebug);
    fpForceFovDeg = GetF(ini, L"FirstPerson", L"ForceFovDeg", fpForceFovDeg);
    fpForcePitchDeg = GetF(ini, L"FirstPerson", L"ForcePitchDeg", fpForcePitchDeg);

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
    patchFpsTarget = GetI(ini, L"Patches", L"FpsTarget", patchFpsTarget);
    patchDpiAware  = GetB(ini, L"Patches", L"DpiAware", patchDpiAware);

    camWatch        = GetB(ini, L"Experimental", L"CamWatch", camWatch);
    camWatchDelay   = GetF(ini, L"Experimental", L"CamWatchDelay", camWatchDelay);
    camWatchSeconds = GetF(ini, L"Experimental", L"CamWatchSeconds", camWatchSeconds);
    camWatchDt      = GetB(ini, L"Experimental", L"CamWatchDt", camWatchDt);
    camWatchIgt     = GetB(ini, L"Experimental", L"CamWatchIgt", camWatchIgt);
    camWatchDst     = GetB(ini, L"Experimental", L"CamWatchDst", camWatchDst);
    igtTrace        = GetB(ini, L"Experimental", L"IgtTrace", igtTrace);
    m2TaskProbe    = GetB(ini, L"Experimental", L"TaskProbe", m2TaskProbe);
    m2ForcePass2   = GetB(ini, L"Experimental", L"ForcePass2", m2ForcePass2);
    m2KillPlayerAfter = GetI(ini, L"Experimental", L"KillPlayerAfter", m2KillPlayerAfter);
    m2Pass2NoGuards = GetB(ini, L"Experimental", L"Pass2NoGuards", m2Pass2NoGuards);
    m2Pass2NoDeadGuard = GetB(ini, L"Experimental", L"Pass2NoDeadGuard", m2Pass2NoDeadGuard);
    taskRegisterTest  = GetB(ini, L"Experimental", L"TaskRegisterTest", taskRegisterTest);
    taskQueueProbe    = GetB(ini, L"Experimental", L"TaskQueueProbe", taskQueueProbe);
    taskDumpLists     = GetB(ini, L"Experimental", L"TaskDumpLists", taskDumpLists);
    taskDoublePumpTest = GetB(ini, L"Experimental", L"TaskDoublePumpTest", taskDoublePumpTest);
    taskContentProbe   = GetB(ini, L"Experimental", L"TaskContentProbe", taskContentProbe);
    taskToggle        = GetS(ini, L"Experimental", L"TaskToggle", taskToggle);
    taskToggleDelay   = GetF(ini, L"Experimental", L"TaskToggleDelay", taskToggleDelay);
    taskToggleSeconds = GetF(ini, L"Experimental", L"TaskToggleSeconds", taskToggleSeconds);
    taskToggleCycles  = GetI(ini, L"Experimental", L"TaskToggleCycles", taskToggleCycles);
    taskToggleMode    = GetS(ini, L"Experimental", L"TaskToggleMode", taskToggleMode);
    autoPilotSeconds = GetI(ini, L"Experimental", L"AutoPilotSeconds", autoPilotSeconds);
    expDumpBackbufferAt = GetF(ini, L"Experimental", L"DumpBackbufferAt", expDumpBackbufferAt);
    autoPilotDelay   = GetF(ini, L"Experimental", L"AutoPilotDelay", autoPilotDelay);
    autoPilotPeriod  = GetF(ini, L"Experimental", L"AutoPilotPeriod", autoPilotPeriod);
    autoPilotButton  = GetS(ini, L"Experimental", L"AutoPilotButton", autoPilotButton);
    autoPilotWiggleSeconds = GetI(ini, L"Experimental", L"AutoPilotWiggleSeconds", autoPilotWiggleSeconds);
    autoPilotMenuSeconds = GetI(ini, L"Experimental", L"AutoPilotMenuSeconds", autoPilotMenuSeconds);
    autoPilotMenuPeriod  = GetF(ini, L"Experimental", L"AutoPilotMenuPeriod", autoPilotMenuPeriod);
    autoPilotMenuButton  = GetS(ini, L"Experimental", L"AutoPilotMenuButton", autoPilotMenuButton);
}

Config& Cfg() { return g_cfg; }
}
