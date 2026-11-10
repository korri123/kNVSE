#pragma once

class NiControllerSequence;
class NiObjectNET;
struct kBlendInterpItem;

// Equip and unequip animations move the Weapon node between the right hand and its holster node at their Attach/Detach
// text key: the engine reparents the node once the animation's time passes the key. The Weapon track switches from keys
// relative to the old parent to keys relative to the new one at the same time, but its last key before the switch is
// relative to the old parent: between those two keys a linear or quadratic track interpolates from one parent's space to
// the other's while the node is still on the old parent, and the weapon flies off for that time (2haunequip.kf, used by
// the Service Rifle: 20 units over 8 ms of animation time, several frames with a low sgtm). Step keyed tracks hold.
// The Weapon track is evaluated in the space of the parent the node has: at its last key before the switch while it is
// still on the old parent, from its first key at the switch once it is on the new one.
namespace WeaponParentSwitch
{
    struct Window
    {
        // the latest time the Weapon track is still relative to the old parent: its last key before the switch
        float fLastOldKey;
        // the switch: the Attach/Detach text key, where the track has its first key relative to the new parent
        float fSwitchKey;
        // Attach: the new parent is the right hand. Detach: the old one is
        bool bToRightHand;
    };

    // Called when a sequence attaches its interpolator for the Weapon node
    void Init(kBlendInterpItem& item, const NiControllerSequence& sequence);
    // The time to evaluate the item's interpolator at instead of fTime, its update time
    float GetUpdateTime(const kBlendInterpItem* item, const NiObjectNET* weapon, float fTime);
}
