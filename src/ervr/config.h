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
    std::string moveForward = "head";     // head (where you look - stick forward walks toward your gaze) | camera (stick passed through: forward = the stick yaw) | hand | body
    std::string rightStick = "camera";    // camera | body: what the right stick turns when ERVR owns the yaw
    bool  handRelativeMovement = false;   // legacy alias for MoveForward=hand
    bool  cameraTurnErvr = true;          // the turn stick drives ERVR's own view yaw (needs CameraYawSource=ervr)
    float turnSpeedDegPerSec = 120.f;     // smooth-turn rate when ERVR owns the yaw
    float snapTurnDegrees = 30.f;         // step size when SnapTurn is on and ERVR owns the yaw
    bool  moveRelativeToCamera = false;   // rotate the move stick by (view yaw - body yaw); ER's stick is camera-relative already, so leave off
    bool  snapTurn = false;               // right stick X -> discrete turns (pulses if ERVR does not own the yaw)
    float snapTurnPulseSeconds = 0.08f;
    float snapTurnRepeatSeconds = 0.30f;

    // [Haptics]
    bool  rumbleToControllers = true;     // forward XInputSetState rumble to Touch controllers
    float rumbleScale = 1.0f;

    // [FirstPerson]  (ERVR takes over the game camera; R.E.A.L. keeps rendering)
    bool  fpEnabled = true;
    float fpEyeHeight = 1.60f;            // meters above the character root (CameraAnchor=root, and the height for CameraAnchor=head)
    std::string fpCameraAnchor = "head";  // head = camera rides on the head bone (body connected by construction) | root = character root + EyeHeight
    float fpCameraAnchorSmoothing = 0.06f;// s: low-pass on the head-bone anchor (kills per-frame animation jitter, keeps the lean)
    float fpCameraBob = 1.f;              // 0..1: how much of the head bone's vertical motion (bob, crouch, run lean) the camera follows (1 = stay on the head)
    // [Locomotion]
    std::string locomotion = "game";      // game = the game moves the character from the stick | direct = ERVR integrates the position/facing itself (no collision from us)
    float locoRunSpeed = 4.5f, locoSprintSpeed = 6.5f;   // m/s for Locomotion=direct
    bool  locoFeedStick = true;           // direct: still hand the stick to the game so run/strafe animations play (its own motion is discarded)
    std::string fpCameraMode = "pre";     // pre | post | block | passthrough (how we hand the camera to the game)
    std::string fpCameraRotation = "yaw"; // yaw | game | hmd  -> what rotation we write into the game camera
    std::string fpCameraYawSource = "ervr"; // ervr (our own yaw, stick-integrated; the game's chr-cam update is replaced so nothing else can move it) | game (read the game camera's yaw back)
    bool  fpOwnChrCam = true;             // replace the game's chr-cam (follow cam) update while first person is on, instead of writing on top of it
    bool  fpOwnHeadPosition = false;      // hide the HMD position from R.E.A.L. (it keeps rotation) and place the eye ourselves (breaks 6DoF feel: off)
    bool  fpAddHmdPosition = false;       // (only when OwnHeadPosition is off/unsupported) add the HMD offset ourselves
    bool  fpHmdYawInGameCamera = false;   // set true if R.E.A.L. writes HMD yaw into the game camera (hands double-turn otherwise)
    float fpRealHmdPositionScale = 1.0f;  // how much of the raw HMD position R.E.A.L. adds to our camera (we subtract it back out)
    bool  fpRecenter = true;              // slow play-space re-centring: without it, standing off the runtime origin makes the eye orbit the body on every stick turn
    float fpRecenterSeconds = 2.0f;       // time constant of the slow re-centring (0 = hard lock, no positional head tracking)
    float fpRecenterSnapRadius = 0.35f;   // m: residual beyond this snaps the play-space origin to your head instead of easing (a runtime origin metres away otherwise parks the body away from the camera and slides it back for seconds)
    float fpRecenterMaxRadius = 0.75f;    // m: never let the head sit further than this from the character
    bool  fpRecenterHeight = true;        // also absorb vertical drift (keeps EyeHeight whatever your real height is)
    bool  fpFaceCamera = true;           // keep the body facing the camera (strafe mode) so arms can always reach
    std::string fpFaceCameraMode = "strafe"; // strafe (Ctrl unlock flag only: no game lock-on camera) | lock (old: force Chr_IsLockedOn + fake target) | off; ini "auto" = strafe with PoseMode=vr, else lock
    float fpLockTargetDistance = 100.f;   // m: how far ahead the fake lock-on point sits (short = the game's lock-on camera drags your yaw)
    std::string fpPoseMode = "vr";        // vr = full-rig template override (no animation ever poses the body; root motion still moves it) | game = animation + IK on top
    std::string fpArmRootSource = "model";  // model = position from the ChrCtrl model matrix (the transform the renderer skins with, read fresh in the model-pos hook) + physics rotation | modelmatrix | physics
    bool  fpPinBodyFacing = true;         // strafe mode: pin the physics facing to BodyYaw so walking toward your gaze is forward speed, not a side-step
    bool  fpCameraAfterModelPos = true;   // write the camera again once the rendered model matrix exists, so camera and body sit on the same transform (kills the body-slides-to-camera lag)
    bool  fpCameraFinalWrite = true;      // replay our camera matrix after the game's camera finalizer (which runs past every other write point and would otherwise render its own interpolated follow position)
    bool  fpLatchVRPerFrame = true;       // take ONE head/hand sample per render cycle and use it at every write point, so the body is posed and the camera placed from the identical head position
    bool  fpPoseAfterHavok = false;       // DISABLED: detouring the Havok pose fn (0x146b5e0) crashes - its ABI is not the guessed 4-arg one (crash dumps at +0x4e). Needs a register-preserving midhook instead. Weapon sway stays until then.
    bool  fpArmSolveEveryCall = true;     // solve the arms on every Havok update, not once per rendered frame
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
    std::string bodyYawSource = "head";   // head (shoulders follow your real head - physically turning turns the body) | camera | stick
    float bodyFacingOffsetDeg = 0.f;      // manual correction if the rendered body faces the wrong way (180 = flipped)
    float bodyYawFollowSeconds = 0.25f;   // time constant of the body chasing the head yaw (0 = instant)
    float bodyYawDeadzoneDeg = 0.f;       // look this far off before the body starts to follow
    bool  torsoFaceCamera = true;         // twist the Spine bone so the chest faces the body yaw
    bool  bodyFollowsHead = true;         // shift the whole rig so its head sits under your eye: physically stepping/leaning moves the character with you (PoseMode=vr makes this idempotent and feedback-free)
    bool  bodyRotateToHead = true;        // also yaw the whole rendered skeleton to BodyYaw (does not wait for the game to turn the character)
    std::string bodyAnchor = "head";      // head = put the head bone at the eye | root = shift by the headset offset only
    float bodyFollowMaxHorizontal = 0.6f; // m clamp for the skeleton shift (character root does not move)
    float bodyFollowMaxDown = 0.6f, bodyFollowMaxUp = 0.25f;
    std::string boneTorso = "Spine", boneHead = "Head";
    std::string boneLUpperArm = "L_UpperArm", boneLForearm = "L_Forearm", boneLHand = "L_Hand";
    std::string boneRUpperArm = "R_UpperArm", boneRForearm = "R_Forearm", boneRHand = "R_Hand";
    bool  dumpSkeleton = true;            // log all bone names once when the player skeleton is first seen
    bool  fpPoseScan = false;             // diagnostic: page-protect the pose buffer for ~1 s and log every reader/writer RVA (one shot per false->true flip)
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
