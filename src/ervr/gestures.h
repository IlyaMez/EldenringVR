#pragma once
#include "vr_backend.h"

namespace ervr {

enum class Swing { None, Light, Heavy, LeftSwing };

// Velocity-based swing detector. Feed it every poll; it returns a swing event
// at most once per cooldown per hand. Peak speed within a short arming window
// decides light vs heavy for the right hand.
class GestureDetector {
public:
    void Reset();
    // Returns the swing that fired this tick (if any) for the given hand.
    Swing Update(Hand hand, const Pose& pose, bool gripHeld, double now);
private:
    struct HandState {
        bool armed = false;
        double armTime = 0;
        float peak = 0;
        float prevSpeed = 0;
        double lastFire = -1e9;
    } m_h[2];
};
}
