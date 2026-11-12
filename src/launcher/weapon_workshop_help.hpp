#pragma once
#include "weapon_presets.h"

// Player-facing copy stays separate from the preset format and runtime terminology.
namespace launcher::workshop {
struct Explanation {
    const char *label;
    const char *meaning;
    const char *example;
};

inline Explanation FieldHelp(ervr::WeaponField field) {
    using F = ervr::WeaponField;
    switch (field) {
    case F::Pitch:
        return {"Tilt up / down",
                "Adds an up/down rotation to your usual weapon alignment. Zero keeps your global calibration.",
                "If the blade points too high, try a small 5-degree adjustment and check it in VR."};
    case F::Yaw:
        return {
            "Turn left / right", "Adds a sideways rotation to your usual weapon alignment.",
            "If the blade points to one side of your controller, try 5 degrees; reverse the sign if it gets worse."};
    case F::Roll:
        return {"Twist the handle",
                "Rotates the weapon around the grip to adjust how the blade or shield sits in your hand.",
                "If the blade edge looks sideways, try a small twist and check it in VR."};
    case F::GripX:
        return {"Move grip sideways",
                "Adds a sideways offset in the weapon's local space. It moves with the weapon as you turn.",
                "0.02 m is a 2 cm correction; use a negative value to move the other way."};
    case F::GripY:
        return {"Move grip up / down",
                "Adds a vertical offset in the weapon's local space, on top of your usual calibration.",
                "Try 0.01 m (1 cm) if the handle does not line up with your palm."};
    case F::GripZ:
        return {"Move grip forward / back",
                "Adds a depth offset in the weapon's local space, on top of your usual calibration.",
                "Try a 0.02 m (2 cm) correction if your hand sits too far along the handle."};
    case F::Reach:
        return {"Distance from hand to tip",
                "Estimated weapon length used for tip speed and weight calculations. This does not resize the weapon "
                "model or its game hitbox.",
                "1.2 m means the estimated tip is 120 cm from the grip. A longer estimate gives faster tip motion for "
                "the same rotation."};
    case F::Balance:
        return {"Balance toward the tip",
                "Estimated balance point as a fraction of the hand-to-tip distance. Higher values put weight farther "
                "from your hand and increase resistance to turning.",
                "0.25 puts the balance a quarter of the way to the tip; 0.75 makes it more tip-heavy."};
    case F::InertiaResponse:
        return {"Follow your hand",
                "How closely the weapon follows your hand's rotation. Higher values feel more responsive. Requires "
                "weapon inertia to be enabled.",
                "Try raising 8 Hz to 12 Hz if the weapon feels slow to catch up."};
    case F::InertiaAcceleration:
        return {"Turning acceleration",
                "Controls how readily the weapon can turn under simulated weight. Higher values make it easier to "
                "move. Requires weapon inertia.",
                "Try raising 300 to 400 rad/s2 if a heavy weapon is too difficult to turn."};
    case F::InertiaBrake:
        return {"Stopping assistance",
                "Extra assistance when the weapon catches up to your hand. Higher values help it settle more quickly. "
                "Requires weapon inertia.",
                "Try 4x instead of 3x if the weapon feels slow to settle when you stop your hand."};
    case F::MaxLag:
        return {"Maximum trailing angle",
                "Limits how far the weapon's rotation can trail behind your controller. Smaller angles keep it closer "
                "to your hand. Requires weapon inertia.",
                "15 degrees keeps the weapon closer than 30 degrees during a fast turn."};
    case F::TwoHandTorque:
        return {"Two-hand turning help",
                "Multiplies turning assistance while you hold the weapon with both hands. Requires weapon inertia.",
                "2x gives more help than 1x when your support hand grabs the weapon."};
    case F::ContactOn:
        return {"Speed to start a strike",
                "Minimum weapon-tip speed that opens a strike in continuous physical combat. Lower values pick up "
                "smaller movements.",
                "Raise 0.8 to 1.2 m/s if small wrist movements start unwanted strikes."};
    case F::ContactOff:
        return {
            "Speed considered at rest",
            "Tip speed below which a continuous strike is treated as resting. Keep this below Speed to start a strike.",
            "At 0.25 m/s, a tip moving at 0.1 m/s is below the rest threshold."};
    case F::ContactDwell:
        return {"End after slowing down",
                "How long a strike can stay slowed before ending. Lower values end the follow-through sooner.",
                "0.12 s means roughly a tenth of a second of slowdown ends the strike."};
    case F::ContactMax:
        return {"Longest strike",
                "Maximum time one continuous strike can stay active, even if the weapon keeps moving.",
                "At 1.5 s, a long sweeping movement cannot keep the same strike open beyond 1.5 seconds."};
    case F::ContactCooldown:
        return {"Pause after a landed strike",
                "Minimum delay after a strike ends before another can start, if it hit something. A missed strike can "
                "rearm immediately.",
                "0.3 s adds a short pause after a landed strike; 0.15 s allows a quicker next strike."};
    case F::SwingOn:
        return {"Speed to start a swing",
                "Tip speed needed for a stance follow-up or gated swing (one attack per detected swing). Continuous "
                "strikes use the separate Physical strikes settings.",
                "Lower 3 to 2.5 m/s if deliberate swings while holding a stance are not being picked up."};
    case F::SwingOff:
        return {"Speed to reset a swing",
                "Tip speed considered resting between stance or gated swings. Keep it below Speed to start a swing.",
                "At 1.5 m/s, slow the tip below that speed before attempting the next stance swing."};
    case F::Heavy:
        return {"Speed for a heavy attack",
                "Peak tip speed used to choose heavy stance follow-ups and non-kinetic gated attacks. Kinetic melee "
                "uses its own energy and arc calculation. Must be at least Speed to start a swing.",
                "Raise this if stance swings choose a heavy follow-up when you intended a light one."};
    case F::MinHand:
        return {"Minimum hand movement",
                "Minimum controller speed for stance and gated swings. Helps reject wrist-only flicks. Continuous "
                "strikes do not require this hand speed.",
                "Raise this if flicking your wrist triggers stance follow-ups without a deliberate arm movement."};
    case F::ArmWindow:
        return {"Time to judge the swing",
                "Time to observe the swing's peak before choosing its attack. Shorter times decide sooner; longer "
                "times allow more of the swing to build.",
                "0.06 s observes about 60 milliseconds before choosing a stance follow-up."};
    case F::SwingCooldown:
        return {"Pause between gated swings",
                "Minimum recovery between attacks in gated combat (one attack per detected swing). Stance follow-ups "
                "use their own pause under Weapon skills.",
                "0.25 s requires a quarter-second recovery before another gated swing."};
    case F::GrabRadius:
        return {
            "How close to grab",
            "How close your free hand must be to the handle to take a support grip. Requires two-hand grab mode.",
            "0.18 m allows a grab within 18 cm of the handle. Increase it if lining up both controllers is awkward."};
    case F::GrabMin:
        return {"Grab zone start",
                "Start of the allowed grip segment measured from your weapon hand along the configured grip axis. With "
                "the default handle axis, negative values extend toward the blade. Must be below Grab zone end.",
                "-0.15 m lets the zone begin 15 cm toward the blade from your weapon hand."};
    case F::GrabMax:
        return {"Grab zone end",
                "End of the allowed grip segment. With the default handle axis, positive values extend toward the "
                "pommel. Must be above Grab zone start.",
                "0.45 m extends the zone 45 cm toward the pommel from your weapon hand."};
    case F::SkillHold:
        return {"Hold to use game skill",
                "How long to hold your configured skill control before it counts as a game-skill hold.",
                "0.25 s means holding for a quarter of a second. Increase it if short taps start the skill "
                "unintentionally."};
    case F::StanceSettle:
        return {"Wait after entering stance",
                "Time to let the weapon settle into a skill stance before a swing can trigger a follow-up. When hold "
                "animation is hidden, this is also the delay between entering the stance and attacking on each swing.",
                "Increase this if moving into a stance accidentally triggers a follow-up."};
    case F::StanceHoldAnimation:
        return {"Stance hold animation",
                "Show while held keeps the game's stance animation playing while you hold the skill control. "
                "Hide while held keeps your weapon free until you swing, then briefly enters the stance and performs "
                "its follow-up. This adds the Wait after entering stance delay to each follow-up. Requires stance "
                "swings on foot; charged skills and skills without follow-ups keep their usual behavior.",
                "For Moonveil, choose Hide while held to avoid staying in the sheathed stance while holding the "
                "trigger. Swing to perform Transient Moonlight. Use default follows your global StanceHoldAnimation "
                "setting."};
    case F::StanceGap:
        return {"Pause between follow-ups",
                "Minimum time between swings that trigger follow-up attacks while holding a stance.",
                "0.4 s keeps stance follow-ups at least four tenths of a second apart."};
    case F::AimClamp:
        return {"Skill aim away from view",
                "Limits how far a skill projectile can aim away from your view when skill aiming follows the swing.",
                "30 degrees keeps a Moonveil wave closer to your view direction than 60 degrees."};
    case F::ShieldHeight:
        return {"Shield height to block",
                "Minimum shield height relative to your eyes. Negative values are below eye level. Requires physical "
                "blocking; the pose detector uses the left-hand shield.",
                "-0.45 m permits blocking 45 cm below your eyes. -0.2 m requires holding it higher."};
    case F::ShieldForward:
        return {"Shield distance in front",
                "How far in front of your head the left-hand shield must be to count as raised. Requires physical "
                "blocking.",
                "0.1 m requires the shield to be at least 10 cm forward. Increase it if holding it near your chest "
                "blocks unintentionally."};
    case F::ShieldRelease:
        return {"Keep blocking after lowering",
                "Delay before releasing block after your shield leaves the raised pose. Helps prevent small movements "
                "from dropping your guard.",
                "0.2 s keeps the guard for a brief dip lasting less than a fifth of a second."};
    case F::DeflectSpeed:
        return {"Shield speed to deflect",
                "Shield speed into an incoming attack needed for a physical deflect at impact. Requires physical "
                "blocking and a correctly timed movement.",
                "Lower 1.2 to 1.0 m/s if you need a gentler forward shield movement to qualify."};
    case F::HapticScale:
        return {"Combat vibration strength",
                "Scales combat vibration in your controllers for this preset. It does not change all game vibration.",
                "1x keeps normal combat vibration, 0.5x halves it, and 0x turns it off."};
    default:
        return {"Unknown setting", "", ""};
    }
}

inline Explanation EventHelp(ervr::WeaponEvent event) {
    using E = ervr::WeaponEvent;
    switch (event) {
    case E::Contact:
        return {"Physical strike / swing",
                "A strike or gated swing detected by ERVR. This is an attack decision, not confirmation that you hit "
                "an enemy.",
                "Choose Light attack to make matching right-hand swings use a light attack."};
    case E::Stance:
        return {"Swing during a skill stance",
                "A swing while holding a skill with follow-up attacks, such as Unsheathe or Moonveil.",
                "Choose Heavy attack and Two-handed to make two-handed stance swings use the heavy follow-up."};
    case E::Skill:
        return {"Use the skill control",
                "Your configured skill button or trigger. The rule chooses which existing skill behavior runs.",
                "Choose Game weapon skill to use the game's skill instead of a recorded effect."};
    case E::Grip:
        return {"Grab with the other hand", "Your free hand taking a support grip. Requires two-hand grab mode.",
                "Choose Skip this action and While riding to suppress support grabs on horseback."};
    case E::Guard:
        return {"Raise the shield", "The left-hand shield entering the blocking pose. Requires physical blocking.",
                "Choose Skip this action to prevent pose-based blocking for this preset."};
    case E::Cast:
        return {"Cast or shoot",
                "Your configured cast or shoot button. This does not create new spells or change ammunition.",
                "Choose Skip this action and While riding to suppress this input on horseback."};
    default:
        return {"Unknown trigger", "", ""};
    }
}
inline Explanation ActionHelp(ervr::WeaponAction action) {
    using A = ervr::WeaponAction;
    switch (action) {
    case A::Default:
        return {"Normal behavior",
                "Let the game and your ERVR settings choose the action. A matching rule still takes priority over "
                "lower-priority rules.",
                "Use this as a higher-priority exception to a rule that skips an action."};
    case A::Suppress:
        return {
            "Skip this action",
            "Suppress this detected action while the rule matches. It does not disable all controls for the weapon.",
            "Skip Raise the shield to stop automatic pose-based blocking."};
    case A::Light:
        return {"Light attack / follow-up",
                "Choose a light right-hand or two-handed attack. An independent left-hand swing keeps its usual "
                "left-hand attack.",
                "Use on a stance swing to request the skill's light follow-up."};
    case A::Heavy:
        return {"Heavy attack / follow-up",
                "Choose a heavy right-hand or two-handed attack. An independent left-hand swing keeps its usual "
                "left-hand attack.",
                "Use on a stance swing to request the skill's heavy follow-up."};
    case A::GameSkill:
        return {"Game weapon skill", "Use the weapon's existing in-game skill through the configured skill control.",
                "For Moonveil, use its normal skill stance and follow-up attacks."};
    case A::EffectSkill:
        return {"Recorded effect skill",
                "Use ERVR's existing recorded effect skill. It still needs the required two-handed setup and a learned "
                "recording.",
                "Use a skill effect you have already recorded in ERVR; selecting this does not record one."};
    default:
        return {"Unknown action", "", ""};
    }
}
inline const char *ConditionLabel(ervr::WeaponCondition condition) {
    switch (condition) {
    case ervr::WeaponCondition::Always:
        return "Any time";
    case ervr::WeaponCondition::OneHand:
        return "One-handed";
    case ervr::WeaponCondition::TwoHand:
        return "Two-handed";
    case ervr::WeaponCondition::Mounted:
        return "While riding";
    case ervr::WeaponCondition::OnFoot:
        return "On foot";
    default:
        return "Unknown condition";
    }
}
} // namespace launcher::workshop
