#pragma once

#include "NiTypes.h"

class NiControllerSequence;
class NiNode;
class NiObjectNET;
class kBlendInterpolatorExtraData;
struct NiQuatTransform;
struct kBlendInterpItem;

// Equip and unequip animations move the Weapon node between the right hand and its holster node at their Attach/Detach
// text key: the engine reparents the node once the animation's time passes the key. The Weapon track switches from keys
// relative to the old parent to keys relative to the new one there, but its last key before the switch is relative to the
// old parent: between those two keys a linear or quadratic track interpolates from one parent's space to the other's while
// the node is still on the old parent, and the weapon flies off for that time (2haunequip.kf, used by the Service Rifle:
// 20 units over 8 ms of animation time, several frames with a low sgtm). Step keyed tracks hold.
// Some tracks switch a key after the text key (vanilla sneak2hrunequip.kf: Detach 0.5, first key relative to the spine
// at 0.5333; pasneak2hrunequip, sneak1hpunequip, 1mdunequip, sneak1mdunequip): for that key the node is on the new
// parent with a pose relative to the old one, the rifle in front of the body turned 131 degrees.
// The Weapon track is evaluated in the space of the parent the node has: at its last key before the switch while it is
// still on the old parent, from its first key relative to the new one once it is there.
// Every other animation of a weapon type (gaits, idles, aims, attacks, reloads of 2hr, 1hp, ...; not the Holster group)
// holds the weapon: its Weapon track is relative to the right hand, and it is left out of the node's blend while the node
// is on another parent. The engine switches gaits and idles to the holstered variant at the Detach key, so the drawn
// ones still fade out under the unequip; normally the unequip outlasts them, but where it ends within a frame of its
// Detach (a 1st person respectEndKey animation's times, or those times taken back when the view switches to 3rd person
// after its Detach time) the fading run held the holstered weapon in its hand pose, sticking out of the back.
namespace WeaponParentSwitch
{
    struct Window
    {
        // the latest time the Weapon track is still relative to the old parent: its last key before the switch
        float fLastOldKey;
        // the track's first key relative to the new parent: at the Attach/Detach text key, or up to a frame after it
        float fSwitchKey;
        // Attach: the new parent is the right hand. Detach: the old one is
        bool bToRightHand;
    };

    // What the holster ease keeps of a Weapon node (kBlendInterpolatorExtraData::parentSwitch)
    struct NodeState
    {
        // the node's parent at its last evaluation, only compared: it may have been freed since
        const NiNode* lastParent = nullptr;
        bool bOnRightHand = false;
        bool bEasing = false;
        // animation time since the node was moved to its holster
        float fElapsed = 0.0f;
        // where the weapon was relative to its holster when it was moved there, minus where its blend put it then
        NiPoint3 translateOffset{};
        NiQuaternion rotateOffset;
        // where the blend put it at the last evaluation of the ease that had a blend: the ease goes on to there when
        // the blend has nothing for the node
        NiPoint3 lastTranslate{};
        NiQuaternion lastRotate;
    };

    // Called when a sequence attaches its interpolator for the Weapon node
    void Init(kBlendInterpItem& item, const NiControllerSequence& sequence);
    // The time to evaluate the item's interpolator at instead of fTime, its update time
    float GetUpdateTime(const kBlendInterpItem* item, const NiObjectNET* weapon, float fTime);
    // Whether the item is blended on the node: not one relative to the right hand while the node is on another parent
    bool Counts(const kBlendInterpItem* item, const NiObjectNET* target);
    // bEaseWeaponHolster: the engine moves the weapon from the hand to its holster at once, wherever the hand is (vanilla
    // 2hrunequip.kf: 7.6 units and 46 degrees). The weapon keeps the place it had in the world when it is moved and goes
    // to where its animation has it over kHolsterEaseTime of animation time. kValue: the Weapon node's blended transform,
    // fDeltaTime: animation time since the blend's last evaluation. Returns whether it gave kValue the transform, the
    // blend having none: the ease goes on after the unequip ended within it
    bool EaseHolster(kBlendInterpolatorExtraData& extraData, NiObjectNET* target, float fDeltaTime, NiQuatTransform& kValue);
}
