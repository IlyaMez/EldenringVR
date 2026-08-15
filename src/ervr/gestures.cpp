#include "gestures.h"
#include "config.h"
#include "log.h"

namespace ervr {

namespace { constexpr double kArmWindow = 0.07; }   // seconds to observe the peak before deciding light/heavy

void GestureDetector::Reset() { m_h[0] = {}; m_h[1] = {}; }

Swing GestureDetector::Update(Hand hand, const Pose& pose, bool gripHeld, double now) {
    const Config& c = Cfg();
    HandState& s = m_h[hand];
    if (!c.gesturesEnabled || !pose.valid) { s.armed = false; return Swing::None; }
    if (c.gestureRequiresGrip && !gripHeld) { s.armed = false; return Swing::None; }

    float speed = pose.linVel.Length();
    float threshold = hand == HandRight ? c.lightAttackSpeed : c.leftSwingSpeed;
    Swing fired = Swing::None;

    if (!s.armed) {
        bool cooled = (now - s.lastFire) >= c.gestureCooldown;
        if (cooled && speed >= threshold && s.prevSpeed < threshold) {
            s.armed = true; s.armTime = now; s.peak = speed;
        }
    } else {
        if (speed > s.peak) s.peak = speed;
        bool decelerating = speed < s.prevSpeed && speed < s.peak * 0.85f;
        bool windowOver = (now - s.armTime) >= kArmWindow;
        if (decelerating || windowOver) {
            s.armed = false;
            s.lastFire = now;
            if (hand == HandRight) fired = (s.peak >= c.heavyAttackSpeed) ? Swing::Heavy : Swing::Light;
            else fired = Swing::LeftSwing;
            LOGD("[gesture] %s hand swing peak %.2f m/s -> %s", hand == HandRight ? "right" : "left", s.peak,
                 fired == Swing::Heavy ? "heavy" : fired == Swing::Light ? "light" : "left");
        }
    }
    s.prevSpeed = speed;
    return fired;
}
}
