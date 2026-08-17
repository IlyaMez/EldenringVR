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
    bool  fpFaceCamera = true;           // keep the body facing the camera (strafe mode) so arms can always reach
    bool  fpHideHead = true;
    bool  fpHideSheathed = false;
    bool  fpWriteAim = true;              // write the HMD look direction into the player's aim matrix (spells/bows)
    bool  handTracking = true;            // drive arm bones from the controllers
    std::string handOrigin = "head";      // head | tracking | floor
    float handBackOffset = 0.06f;         // meters from controller grip pose back to the wrist bone
    float handRotOffX = 0.f, handRotOffY = 0.f, handRotOffZ = 0.f;   // degrees, controller -> hand bone calibration
    float elbowPoleDown = 1.0f, elbowPoleOut = 0.6f, elbowPoleBack = 0.3f;
    float armStretch = 1.5f;              // max stretch factor of forearm+hand when the target is out of reach (1 = none)
    float handScale = 1.0f;               // scale controller offsets from the head (compensate arm length differences)
    bool  torsoFaceCamera = true;         // twist the Spine bone so the chest faces the camera yaw
    bool  bodyFollowsHead = true;         // shift the whole skeleton by the HMD offset so shoulders stay under your head
    float bodyFollowMaxHorizontal = 0.6f; // m clamp for the skeleton shift (character root does not move)
    float bodyFollowMaxDown = 0.6f, bodyFollowMaxUp = 0.25f;
    std::string boneTorso = "Spine";
    std::string boneLUpperArm = "L_UpperArm", boneLForearm = "L_Forearm", boneLHand = "L_Hand";
    std::string boneRUpperArm = "R_UpperArm", boneRForearm = "R_Forearm", boneRHand = "R_Hand";
    bool  dumpSkeleton = true;            // log all bone names once when the player skeleton is first seen
    bool  fpDebug = false;                // per-second debug lines with frame data

    // [Combat]  physical melee: swings invoke the game's AttackBehavior event instead of pressing R1/R2
    bool  combatEnabled = true;
    std::string combatMode = "auto";      // buttons | physical | auto (physical once the light attack judge id is known)
    float swingOnSpeed = 3.0f;            // m/s weapon-tip speed that starts a swing
    float swingOffSpeed = 1.5f;           // m/s tip speed below which the swing ends
    float heavySwingSpeed = 6.0f;         // m/s peak tip speed (during the arm window) that makes it a heavy attack
    float minHandSpeed = 0.8f;            // m/s controller (not tip) speed required to start (rejects wrist flicks)
    float weaponReachRight = 0.8f, weaponReachLeft = 0.5f;   // m from controller to weapon tip (lever arm for tip speed)
    float swingArmWindow = 0.06f;         // s to observe the peak before choosing light/heavy
    float swingMaxSeconds = 0.6f;         // s hard limit for one attack instance
    float swingCooldown = 0.25f;          // s after a swing before another can start
    bool  combatLeftHand = true;          // left-hand swings attack with the left weapon (source = left)
    bool  combatRequireStamina = true;
    int   combatMinStamina = 1;
    // Judge ids observed in ER 1.16: R1 chain = 0/10/20, R2 = 100 (105 charged variant), left hand = 0 with source 2.
    int   lightJudgeId = 0, heavyJudgeId = 100, leftJudgeId = 0;
    bool  combatLearn = true;             // refine the ids from the game's own events after our button presses
    bool  combatSaveLearned = false;      // opt-in: write learned ids back into ERVR.ini
    bool  combatBlockAttackButtons = true;// while physical: never send RB/RT (trigger/grip would play attack anims)
    int   combatAttackIndexRotate = 0;    // diagnostic: rotate AttackIndex 0..N-1 per swing (0 = off)
    int   combatExtraStamina[3] = {0, 0, 0};   // stamina taken by ERVR per light/heavy/left swing (0 = only the game's own cost)
    bool  combatSuppressAnimAttacks = false;   // swallow AttackBehavior events from animations while physical
    bool  combatDebug = false;
    std::wstring iniPath;                 // where Load() read from (for SaveLearned)

    // [Cheats]  offline convenience for testing (writes the save's PlayerGameData)
    int   cheatSetAllStats = 0;           // >0: raise every stat below this value to it (level adjusted); 0 = off

    // [MenuNav]  left stick acts as a held D-pad while a menu is open (game auto-repeats), gestures muted
    bool  menuNavEnabled = true;
    float menuNavThreshold = 0.55f;       // stick deflection that counts as a D-pad press (hysteresis: releases at 0.6x)
    bool  menuNavFollowStart = true;      // no memory flag: enter on our START press, leave on the next START/BACK
    float menuNavToggleHold = 0.5f;       // hold left stick click (no tilt) this long -> toggle menu mode; 0 = off
    bool  menuNavHaptics = true;
    std::string menuAob = "48 8B 0D ?? ?? ?? ?? 48 8B 53 08 48 8B 92 D8 00 00 00 48 83 C4 20 5B";  // CSMenuMan* global
    int   menuAobOffset = 3, menuAobInstrLen = 7;
    std::string menuChain;                // pointer chain from the CSMenuMan* global to a byte that is nonzero while a menu is open; empty = disabled
    bool  menuScan = false;               // debug: log which bytes of the CSMenuMan object change (find MenuChain)
    int   menuScanSize = 0x400;

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
