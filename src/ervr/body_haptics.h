#pragma once
// Body haptics: what happens to the Tarnished, felt on bHaptics devices - a TactSuit (the vest), a
// TactSleeve on each forearm and a TactVisor on the headset. Blows given and blocked in the arm
// that gave them, blows taken on the side they came from, heals, falls and landings, the heart at
// low HP, and the game's own rumble on the body.
//
// body_haptics_er.cpp posts what happened (BodyEvent) and, every tick, what is going on (BodyState);
// BodyMixer turns both into each motor's strength for one moment (BodyFrame), which
// bhaptics_output.cpp sends to the bHaptics Player. Every pattern is made here, from code: nothing
// is designed or deployed in bHaptics' own portal. Pure and deterministic: tools/test_body_haptics.cpp
// plays it.
//
// Ported from Spidy (../Spidy/src/body_haptics.cpp, same author, GPL-3), whose strengths were tuned on
// the bHaptics devkit with bHaptics' own notes (2026-10-20 / 22). The first session there had most
// events at 50-100%: "way too agressive, directionality didnt really feel like anything, felt like 0
// to 100 always". Since then a pattern is a spot of a few motors where it can be, what goes on all
// the time (rumble, wind, the heart) stays under a third and grows from nothing, and events play at
// 10-45% but for the heaviest blows. ELDEN RING's own patterns (strike, block, deflect, cast, death)
// are built from the same measures, untuned on a suit yet.
#include "vrmath.h"
#include <array>
#include <cstdint>
#include <vector>

namespace ervr {

// The devices, as the bHaptics SDK places them (playDot's `position`: 0 the vest, 1 and 2 the left
// and right forearm, 3 the head).
enum class BodyDevice : uint8_t { Vest, LeftArm, RightArm, Head };
constexpr unsigned kBodyDevices = 4, kBodyMaxMotors = 32;
constexpr unsigned BodyMotorCount(BodyDevice d) { return d == BodyDevice::Vest ? 32u : d == BodyDevice::Head ? 4u : 3u; }
constexpr int32_t BodySdkPosition(BodyDevice d) { return (int32_t)d; }
constexpr BodyDevice BodyArm(int hand) { return hand == 1 ? BodyDevice::RightArm : BodyDevice::LeftArm; }

// Each motor's strength, 0..1, by device and the device's own motor index.
struct BodyFrame {
    std::array<std::array<float, kBodyMaxMotors>, kBodyDevices> motors{};
    float& At(BodyDevice d, unsigned m) { return motors[(unsigned)d][m]; }
    float At(BodyDevice d, unsigned m) const { return motors[(unsigned)d][m]; }
    bool Any(BodyDevice d) const;   // whether any motor of the device is on
};

// The TactSuit's motors (bHaptics' motor index, docs.bhaptics.com/sdk/further/motor): 0-15 the front,
// 16-31 the back, each a grid of four rows (0 the shoulders, 3 the waist) by four columns, row by
// row. On both sides the diagram puts column 0 at the wearer's left: the front seen from ahead and
// the back seen from behind (Spidy's devkit agreed, 2026-10-20). If the suit says otherwise in the
// hand, these two flip a side.
constexpr bool kBodyFrontMirrored = false, kBodyBackMirrored = false;
// The motor on `back` (else the front), `row` 0-3 down from the shoulders, `side` 0-3 across from the
// wearer's left to the right.
constexpr unsigned BodyVestMotor(bool back, unsigned row, unsigned side) {
    return (back ? 16u : 0u) + row * 4 + ((back ? kBodyBackMirrored : kBodyFrontMirrored) ? 3 - side : side);
}
// Where a vest motor sits: around the body, radians from straight ahead, positive to the wearer's
// right (the back's middle at +-pi); down the body, 0 at the shoulders to 1 at the waist.
float BodyVestAzimuth(unsigned motor);
float BodyVestHeight(unsigned motor);
// The TactVisor's four motors run across the forehead, from the wearer's left (as assumed).
constexpr bool kBodyVisorMirrored = false;
constexpr unsigned BodyVisorMotor(unsigned fromLeft) { return kBodyVisorMirrored ? 3 - fromLeft : fromLeft; }

// What happened. Each makes a pattern of its own, a fraction of a second long.
enum class BodyKind : uint8_t {
    Strike,    // a blow of the player's weapon landed (hand, -1 both: two-handed; strength: the swing's quality)
    Block,     // the raised shield took a hit (hand)
    Deflect,   // ... met moving into it: a deflect (hand)
    TwoHand,   // both hands closed on the weapon
    OneHand,   // ... and one let go of it
    Winded,    // a swing refused: no stamina left for it
    Cast,      // a spell, an incantation, an arrow or a throw left the player (hand)
    Hit,       // the player was hit (azimuth, height; strength: how much of the HP it took)
    Heal,      // HP came back at once: a flask, a heal (strength)
    Death,     // HP reached 0
    Landing,   // feet down after a fall (strength: how hard)
    Hello,     // the devices were found: the whole body twice, softly - "they work"
};
constexpr unsigned kBodyKinds = (unsigned)BodyKind::Hello + 1;
const char* BodyKindName(BodyKind);   // for the log and the test tool ("strike", ...)
float BodyKindDuration(BodyKind);     // seconds the pattern plays

// What the [bHaptics] switches play or silence as one: combat (blows given, blocks, deflects, the
// two-hand grab, casts), damage (blows taken, heals, death and the heart at low HP), movement (falls
// and landings) and the game's rumble. Hello plays whenever body haptics do.
enum class BodyGroup : uint8_t { Combat, Damage, Movement, Rumble, Always };
constexpr uint32_t kBodyAllGroups = 0xf;
BodyGroup BodyKindGroup(BodyKind);

struct BodyEvent {
    BodyKind kind{};
    int8_t hand = -1;     // 0 the left, 1 the right; -1 neither (or both)
    float strength = 1;   // 0..1
    float azimuth = 0;    // where it comes from around the body: radians from ahead, + to the right
    float height = .4f;   // and how high: 0 the shoulders to 1 the waist
    // Whether azimuth and height say anything: false plays the event's own place (a hit: the chest
    // and the back at once).
    bool placed = true;
};

// What goes on while it lasts, given every tick.
struct BodyState {
    // The player is in the world (not a loading screen, not the title): off, everything stops and
    // events are let go of - all but Hello.
    bool live = false;
    // The player's velocity in the frame of the torso: x to the right, y up, z ahead; m/s. In a fall
    // the wind blows against it.
    Vec3 velocity{};
    bool airborne = false;   // the game's own is_falling
    // The game's rumble (XInputSetState), 0..1: [0] the heavy left motor, [1] the light right one.
    std::array<float, 2> rumble{};
    float health = 1;        // HP / max HP; 1 where unknown
    uint32_t groups = kBodyAllGroups;   // bit 1 << BodyGroup: one off plays none of its events, and stops at once
};

// Where the torso faces, which the vest does: it is not tracked. It faces where the head does until
// the head turns further than a look over the shoulder (kDeadZone), then turns with it at up to kSpeed
// radians a second, and drifts after it by kDrift of the way a second meanwhile. Spidy's vest faced
// wherever the head looked until 2026-10-20, so a look around turned every blow round the body with it.
// Yaws in the game's convention (radians, positive to the right) in the headset's room; the room's
// turn into the world (ERVR's stick yaw) is added by the caller.
class BodyFacing {
public:
    static constexpr float kDeadZone = .6f, kSpeed = 6.f, kDrift = .5f;
    float Update(float headYaw, float seconds);   // the torso's yaw in the room
    void Reset() { *this = {}; }
private:
    float yaw_ = 0;
    bool set_ = false;
};
// How high on the body a blow from `rise` metres above the player's feet, `flat` metres away, lands
// (BodyEvent::height): level at the chest, from above on the shoulders, from below at the waist.
float BodyHitHeight(float rise, float flat);

class BodyMixer {
public:
    void Post(const BodyEvent& e, double now);   // plays `e` from `now` (seconds on a steady clock)
    void Set(const BodyState& s);
    // Plays `f` as it is for `seconds` from `now`, over everything else (the test tool's motor map).
    void Hold(const BodyFrame& f, double now, float seconds);
    // Every motor at `now`, which only goes forward: 0..1 each, before the player's level.
    BodyFrame Render(double now);
    size_t Playing() const { return active_.size(); }   // patterns playing, for the tests
private:
    struct Active { BodyEvent e; double start = 0; };
    std::vector<Active> active_;
    struct Held { BodyFrame frame; double until = 0; };
    std::vector<Held> held_;
    BodyState state_;
    std::array<float, 2> rumbleFelt_{};   // the rumble and the wind as felt, smoothed
    float windFelt_ = 0;
    double last_ = -1;
};

// The end of a fall: how hard the feet came down (0..1; 0 for none, or one too soft to feel).
class BodyLandingWatch {
public:
    // seconds: since the last call; airborne: the game's is_falling; downSpeed: m/s, the world's.
    float Update(float seconds, bool airborne, float downSpeed);
    void Reset() { *this = {}; }
private:
    bool airborne_ = false;
    float fastest_ = 0, airSeconds_ = 0;
};

}
