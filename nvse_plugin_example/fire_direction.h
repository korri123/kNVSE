#pragma once

struct NiQuaternion;
struct NiQuatTransform;
class kBlendInterpolatorExtraData;

// Where the player's shots go in third person. TESObjectWEAP::Fire (0x523150) aims a shot along the world rotation of
// the weapon's projectile node for every shooter but the player in first person (heading and pitch there); NPCs with a
// combat target then aim at it from the node instead. The player's projectile is then turned towards the point under
// the crosshair (Projectile::AimProjectile 0x9BD860 -> PlayerCharacter::ApplyAutoAimActor), by at most
// fAutoAimMaxDegrees3rdPerson (10) per axis: a shot aimed further off than that misses the crosshair by the rest.
// So in third person the player's shots follow whatever turns the weapon, also what kNVSE adds to its blend: blend
// smoothing taking up a jump, additive animations and WeaponHandBlend. Smoothing: the hip pose is lowered 62 degrees
// and an attack blends in over 1 frame; from the steady pose that passes unsmoothed (the barrel is within 1 degree
// when the shot fires, ~20 ms later), but an attack that starts while aim <-> aimIS still blends cuts those sequences,
// which smoothing takes up as a jump: the weapon comes up over ~0.2 s, the barrel is 19-27 degrees low at the shot and
// the shot missed the crosshair by 8-22 degrees.
// Each bone's blend records the rotation of its computed weights (kNVSE's blend before smoothing takes any up,
// AimPitch included, no additive animations, no WeaponHandBlend; not what the game would blend without kNVSE)
// wherever that differs from what was written, and the player's shots in third person are aimed along the projectile
// node turned by those differences of the bones above it. Where nothing differs Fire gets the node's own transform.
// The shot still starts at the projectile node where it is shown.
namespace FireDirection
{
    // After a bone's blend (NiBlendTransformInterpolator::UpdateHooked): kComputedRotate is the rotation of its
    // computed weights (invalid if none), kValue what is written to the node
    void Record(kBlendInterpolatorExtraData& extraData, const NiQuaternion& kComputedRotate, const NiQuatTransform& kValue);

    void WriteHooks();
}
