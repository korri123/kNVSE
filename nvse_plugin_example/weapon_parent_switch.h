#pragma once

#include "NiTypes.h"

class NiControllerSequence;
class NiNode;
class NiObjectNET;
class kBlendInterpolatorExtraData;
struct NiQuatTransform;
struct kBlendInterpItem;

// Equip and unequip animations move the Weapon node between the right hand and its holster node at their
// Attach/Detach text key: the engine reparents the node once the animation time passes the key. The Weapon track
// changes from keys relative to the old parent to keys relative to the new one at that point. Its last key before the
// switch is still relative to the old parent, so a linear or quadratic track interpolates between the two spaces while
// the node is on the old parent, and the weapon flies off briefly. Step keyed tracks hold and are not affected.
// Some tracks switch one key after the text key (e.g. vanilla sneak2hrunequip.kf). For that key the node is already on
// the new parent but the pose is relative to the old one.
//
// The Weapon track is therefore evaluated in the space of the parent the node currently has: at its last key before
// the switch while the node is on the old parent, and from its first key relative to the new parent once it is there.
//
// Every other animation of a weapon type (gaits, idles, aims, attacks, reloads; not the Holster group) holds the
// weapon: its Weapon track is relative to the right hand. It is left out of the node's blend while the node is on
// another parent. The engine switches gaits and idles to the holstered variant at the Detach key, so the drawn ones
// still fade out under the unequip. Normally the unequip outlasts them, but if it ends within a frame of its Detach
// (a first person respectEndKey animation, or one whose times are moved when the view changes to third person), the
// fading ones would hold the holstered weapon in its hand pose.
namespace WeaponParentSwitch
{
    struct Window
    {
        // Latest time the Weapon track is still relative to the old parent: its last key before the switch
        float fLastOldKey;
        // The track's first key relative to the new parent: at the Attach/Detach text key, or up to a frame after it
        float fSwitchKey;
        // Attach: the new parent is the right hand. Detach: the old parent is
        bool bToRightHand;
    };

    // What the holster ease keeps of a Weapon node (kBlendInterpolatorExtraData::parentSwitch)
    struct NodeState
    {
        // The node's parent at its last evaluation. Only compared, never dereferenced: it may have been freed.
        const NiNode* lastParent = nullptr;
        bool bOnRightHand = false;
        bool bEasing = false;
        // Animation time since the node was moved to its holster
        float fElapsed = 0.0f;
        // Where the weapon was relative to its holster when it was moved there, minus where its blend put it then
        NiPoint3 translateOffset{};
        NiQuaternion rotateOffset;
        // Where the blend put the node at the last evaluation that had a blend. The ease continues to there when the
        // blend has nothing for the node.
        NiPoint3 lastTranslate{};
        NiQuaternion lastRotate;
    };

    // Called when a sequence attaches its interpolator for the Weapon node
    void Init(kBlendInterpItem& item, const NiControllerSequence& sequence);
    // Returns the time to evaluate the item's interpolator at instead of fTime, its update time
    float GetUpdateTime(const kBlendInterpItem* item, const NiObjectNET* weapon, float fTime);
    // Returns true if the item is blended on the node: not one relative to the right hand while the node is on another parent
    bool Counts(const kBlendInterpItem* item, const NiObjectNET* target);
    // bEaseWeaponHolster: the engine moves the weapon from the hand to its holster at once, wherever the hand is. The
    // weapon keeps its world pose at that moment and moves to where its animation has it over kHolsterEaseTime of
    // animation time. kValue is the Weapon node's blended transform, fDeltaTime the animation time since the blend's
    // last evaluation. Returns true if it set kValue although the blend had no transform: the ease continues after the
    // unequip ended within it.
    bool EaseHolster(kBlendInterpolatorExtraData& extraData, NiObjectNET* target, float fDeltaTime, NiQuatTransform& kValue);
}
