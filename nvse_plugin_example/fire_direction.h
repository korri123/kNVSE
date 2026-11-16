#pragma once

struct NiQuaternion;
struct NiQuatTransform;
class kBlendInterpolatorExtraData;

// Where the player's shots go in third person. TESObjectWEAP::Fire (0x523150) aims a shot along the world rotation of
// the weapon's projectile node (heading and pitch) for every shooter except the player in first person. NPCs with a
// combat target then aim at it from the node instead. The player's projectile is turned towards the point under the
// crosshair (Projectile::AimProjectile 0x9BD860 -> PlayerCharacter::ApplyAutoAimActor) by at most
// fAutoAimMaxDegrees3rdPerson (10) per axis, so a shot aimed further off than that misses the crosshair.
// In third person the player's shots therefore follow whatever rotates the weapon, including what kNVSE adds to its
// blend: blend smoothing taking up a jump, additive animations and WeaponHandBlend. Smoothing in particular lowers
// the barrel while an attack starts during an aim <-> aimIS blend, which it treats as a jump.
//
// To avoid that, each bone's blend records the rotation of its computed weights (kNVSE's blend before smoothing,
// including AimPitch, excluding additive animations and WeaponHandBlend) wherever that differs from what was written.
// The player's shots in third person are aimed along the projectile node rotated by those differences for the bones
// above it. Where nothing differs, Fire gets the node's own transform. The shot still starts at the projectile node.
namespace FireDirection
{
    // Called after a bone's blend (NiBlendTransformInterpolator::UpdateHooked). kComputedRotate is the rotation of its
    // computed weights (invalid if none), kValue is what is written to the node.
    void Record(kBlendInterpolatorExtraData& extraData, const NiQuaternion& kComputedRotate, const NiQuatTransform& kValue);

    void WriteHooks();
}
