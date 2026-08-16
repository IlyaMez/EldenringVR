#pragma once
#include <string>
#include <cstdint>

namespace ervr {

// Pseudo XInput masks used by virtual_pad for digital trigger mappings.
// (0x0400 and 0x0800 are unused by XInput; we take 0x0400 for RT and 0x0800 for LT.)
constexpr uint16_t VP_RT = 0x0400;
constexpr uint16_t VP_LT = 0x0800;

// XInput button names accepted in the ini (case-insensitive). 0 if none/unknown.
uint16_t ParseXInputButton(const std::string& name);
const char* XInputButtonName(uint16_t mask);

struct Config {
    // [General]
    std::string logLevel = "info";
    std::string vrRuntime = "auto";        // auto | oculus | openvr
    bool enabled = true;
    int  pollHz = 250;

    // [Buttons]  Touch -> XInput. Values are XInput button names or "none".
    std::string btnA = "A", btnB = "B", btnX = "X", btnY = "Y";
    std::string btnLThumb = "LS", btnRThumb = "RS", btnMenu = "START";
    std::string btnLTrigger = "LB", btnRTrigger = "RB";
    std::string btnLGrip = "LT",  btnRGrip = "RT";
    float triggerThreshold = 0.55f;
    float gripThreshold = 0.60f;
    float menuHoldForBack = 0.45f;         // hold Menu this long -> BACK (map) instead of START
    bool  leftGripPlusStickIsDpad = true;  // hold left grip: left stick acts as D-pad (quick slots)

    // [Sticks]
    float stickDeadzone = 0.15f;
    bool  swapSticks = false;
    float rightStickSensitivity = 1.0f;

    // [Gestures]
    bool  gesturesEnabled = true;
    float lightAttackSpeed = 2.2f;     // m/s (right hand)
    float heavyAttackSpeed = 4.0f;     // m/s
    float leftSwingSpeed   = 2.4f;     // m/s (left hand)
    std::string lightAttackButton = "RB";
    std::string heavyAttackButton = "RT";
    std::string leftSwingButton   = "LB";
    float gestureCooldown = 0.35f;     // s between triggered swings per hand
    float gesturePressDuration = 0.09f;// s a synthesized button is held
    bool  gestureRequiresGrip = false; // only detect swings while grip held
    bool  gestureHaptics = true;

    // [Movement]
    bool  handRelativeMovement = false;   // left stick direction relative to left controller yaw (vs HMD yaw)
    bool  snapTurn = false;               // right stick X -> discrete pulses
    float snapTurnPulseSeconds = 0.08f;
    float snapTurnRepeatSeconds = 0.30f;

    // [Haptics]
    bool  rumbleToControllers = true;     // forward XInputSetState rumble to Touch controllers
    float rumbleScale = 1.0f;

    // [FirstPerson]  (ERVR takes over the game camera; R.E.A.L. keeps rendering)
    bool  fpEnabled = true;
    float fpEyeHeight = 1.60f;            // meters above the character root
    std::string fpCameraMode = "pre";     // pre | post | block | passthrough (how we hand the camera to the game)
    std::string fpCameraRotation = "yaw"; // yaw | game | hmd  -> what rotation we write into the game camera
    bool  fpAddHmdPosition = false;       // add the HMD positional offset ourselves (R.E.A.L. usually does)
    bool  fpHmdYawInGameCamera = false;   // set true if R.E.A.L. writes HMD yaw into the game camera (hands double-turn otherwise)
    bool  fpHideHead = true;
    bool  fpHideSheathed = true;
    bool  fpWriteAim = true;              // write the HMD look direction into the player's aim matrix (spells/bows)
    bool  handTracking = true;            // drive arm bones from the controllers
    std::string handOrigin = "head";      // head | tracking | floor
    float handBackOffset = 0.06f;         // meters from controller grip pose back to the wrist bone
    float handRotOffX = 0.f, handRotOffY = 0.f, handRotOffZ = 0.f;   // degrees, controller -> hand bone calibration
    float elbowPoleDown = 1.0f, elbowPoleOut = 0.6f, elbowPoleBack = 0.3f;
    std::string boneLUpperArm = "L_UpperArm", boneLForearm = "L_Forearm", boneLHand = "L_Hand";
    std::string boneRUpperArm = "R_UpperArm", boneRForearm = "R_Forearm", boneRHand = "R_Hand";
    bool  dumpSkeleton = true;            // log all bone names once when the player skeleton is first seen
    bool  fpDebug = false;                // per-second debug lines with frame data

    // [Experimental] hand-aim: rotates the game camera toward where the right hand points
    bool  handAimEnabled = false;
    std::string cameraAob;                // AOB for a rip-relative LEA/MOV to the camera manager pointer
    int   cameraAobOffset = 3;            // byte offset of the rel32 inside the pattern
    int   cameraAobInstrLen = 7;          // total instruction length for rip-relative resolve
    std::string cameraChain;              // e.g. "0x20,0x60" pointer chain from resolved base to the camera struct
    int   cameraYawOffset = 0;            // offset of float yaw (radians) inside the camera struct
    int   cameraPitchOffset = 4;
    float handAimStrength = 1.0f;

    void Load(const std::wstring& iniPath);
};

Config& Cfg();
}
