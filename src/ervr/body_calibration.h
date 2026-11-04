#pragma once
// The T-pose body calibration in the headset (body_calibration_logic.h: what counts as the pose, and what the
// measurements change). The first time a session shows first-person play without a calibration (after 1.5 s of
// it), a panel appears 1.4 m ahead, where you face then, and stays in the room: stand tall, look ahead, arms
// straight out to the sides, hold both triggers. A ring at each controller turns gold while that arm is in place
// and fills with the hold; after 1.5 s the panel shows your height and arm length for 3.5 s and both controllers
// buzz. B skips it, and ERVR asks no more ([Body] CalibrationPrompt=false). A menu, a cutscene, a load, Torrent or
// the big screen holds it until play returns.
//
// The result goes into ERVR.ini [Body] (EyeHeightMm, ArmLengthMm), where the launcher shows it; its Redo clears
// them, and ERVR asks again at the next moment of play - mid-game too, through the ini's hot reload.
//
// Threads: BodyCalibrationUpdate on the pad worker; the getters from any thread.
#include "vr_backend.h"
#include <cstdint>

namespace ervr {

struct CalibPadOut {
    // From the panel's start until each is let go after it, these do nothing in the game (no attack, guard, jump,
    // dodge, weapon wheel, belt or two-hand grab); the sticks, the menu button, X and Y work as always.
    bool trigger[2] = {false, false}, grip[2] = {false, false};
    uint32_t buttons = 0;          // VRB_ bits held back (A, B)
    bool showing = false;          // the panel is up: physical combat and swing gestures stand down
    float hapticAmp[2] = {0.f, 0.f}, hapticSec[2] = {0.f, 0.f};
};
// Pad worker, once per update, before anything reads the controls. `menu` = any menu, the map or menu-nav mode.
CalibPadOut BodyCalibrationUpdate(const VRTracking& tr, const VRInputState& in, double now, bool menu);
// Ask for a calibration at the next moment of play, whatever [Body] holds (Numpad *, the ERVR tab's Body
// calibration row); the old one stays in use until the new one is done, and B keeps it. `ask` false takes back an
// ask whose panel has not been answered yet (the row turned back before the menu closed). Any thread.
void BodyCalibrationRequest(bool ask = true);

// The measurements in use, metres; 0 = none. ERVR.ini [Body], or one just taken until the hot reload brings it.
float BodyEyeHeight();
float BodyArmLength();

}
