#include "weapon_parent_switch.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string_view>

#include "blend_smoothing.h"
#include "game_types.h"
#include "hooks.h"
#include "NiObjects.h"
#include "string_view_util.h"

namespace
{
    // Tolerance for a key at the text key's time
    constexpr float kSameKeyTime = 1e-3f;
    // How late a track may switch parent space: one key of a 30 fps KF
    constexpr float kMaxLateSwitch = 1.0f / 30.0f + 2e-3f;
    // A later key counts as the switch only if the track jumps this many times as far there as at the text key. A
    // change of space is a large jump in a track that otherwise moves a little per key.
    constexpr float kLateSwitchRatio = 4.0f;
    // Animation time the weapon takes to move from where the hand let it go to its holster pose
    constexpr float kHolsterEaseTime = 0.15f;

    const NiFixedString& RightHandName()
    {
        static const NiFixedString name("Bip01 R Hand");
        return name;
    }

    struct TrackSwitch
    {
        float fLastOldKey = INFINITY;
        float fSwitchKey = -INFINITY;
    };

    // Finds where a track switches parent space: at its key at the text key, or at a later key within kMaxLateSwitch
    // that jumps much further. Tracks without a key at the text key are ignored. getStep(i) is how far the track
    // moves from key i - 1 to key i, in any unit of its own.
    template <typename GetTime, typename GetStep>
    void AddTrack(unsigned int numKeys, GetTime&& getTime, GetStep&& getStep, float fTextKey, TrackSwitch& result)
    {
        unsigned int atTextKey = 0;
        unsigned int largest = 0;
        float fLargestStep = -1.0f;
        for (unsigned int i = 1; i < numKeys; ++i)
        {
            const float fKeyTime = getTime(i);
            if (fKeyTime < fTextKey - kSameKeyTime)
                continue;
            if (fKeyTime > fTextKey + kMaxLateSwitch)
                break;
            if (!atTextKey && fKeyTime <= fTextKey + kSameKeyTime)
                atTextKey = i;
            if (const float fStep = getStep(i); fStep > fLargestStep)
            {
                largest = i;
                fLargestStep = fStep;
            }
        }
        if (!atTextKey)
            return;
        const unsigned int switchKey = largest != atTextKey && fLargestStep > kLateSwitchRatio * getStep(atTextKey) ? largest : atTextKey;
        result.fLastOldKey = (std::min)(result.fLastOldKey, getTime(switchKey - 1));
        result.fSwitchKey = (std::max)(result.fSwitchKey, getTime(switchKey));
    }

    float RotationStep(const NiQuaternion& a, const NiQuaternion& b)
    {
        return 2.0f * std::acos((std::min)(1.0f, std::abs(NiQuaternion::Dot(a, b))));
    }

    // BSAnimGroupSequence::InitAnimGroup (0x5F3A20) reads a text key line by line and takes a line starting with the
    // action name, in any case (strnicmp over the name's length). Returns true for Attach (to the right hand).
    std::optional<bool> ParseSwitchKey(std::string_view text)
    {
        for (size_t pos = 0; pos < text.size();)
        {
            const auto end = text.find_first_of("\r\n", pos);
            const auto line = text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
            if (sv::starts_with_ci(line, "Attach"))
                return true;
            if (sv::starts_with_ci(line, "Detach"))
                return false;
            if (end == std::string_view::npos)
                break;
            pos = end + 1;
        }
        return std::nullopt;
    }

    // The engine acts on a text key by the group's action: only equip and unequip animations move the Weapon node at
    // their Attach/Detach key. A throw attack's Attach is where the next projectile appears in the hand, which the node
    // never left.
    bool MovesWeapon(const NiControllerSequence& sequence)
    {
        if (NOT_TYPE(&sequence, BSAnimGroupSequence))
            return false;
        const TESAnimGroup* animGroup = static_cast<const BSAnimGroupSequence&>(sequence).animGroup;
        if (!animGroup)
            return false;
        const auto groupId = animGroup->GetBaseGroupID();
        return groupId == kAnimGroup_Equip || groupId == kAnimGroup_Unequip;
    }

    // Returns true for animations of a weapon type that hold the weapon (relative to the right hand): every one except
    // equip, unequip (which move it, see parentSwitch) and the Holster group (weapon in its holster). Groups without
    // a weapon type (mt, special idles) are ignored because their Weapon track's space is unknown.
    bool HoldsWeapon(const NiControllerSequence& sequence)
    {
        if (NOT_TYPE(&sequence, BSAnimGroupSequence))
            return false;
        const TESAnimGroup* animGroup = static_cast<const BSAnimGroupSequence&>(sequence).animGroup;
        if (!animGroup || AnimGroup::GetHandType(animGroup->groupID) == kAnimHandType_None)
            return false;
        const auto groupId = animGroup->GetBaseGroupID();
        return groupId != kAnimGroup_Equip && groupId != kAnimGroup_Unequip && groupId != kAnimGroup_Holster;
    }

    // Returns true if an unequip animation is attached to the node: the engine moved it at its Detach key, not a script or menu
    bool HasDetachItem(const kBlendInterpolatorExtraData& extraData)
    {
        return std::ranges::any_of(extraData.items, [](const kBlendInterpItem& item)
        {
            return item.sequence && !item.detached && item.hasParentSwitch && !item.parentSwitch.bToRightHand;
        });
    }

    NiQuaternion UnitRotation(const NiMatrix33& rotation)
    {
        NiQuaternion result;
        result.FromRotation(rotation);
        result.Normalize();
        return result;
    }
}

void WeaponParentSwitch::Init(kBlendInterpItem& item, const NiControllerSequence& sequence)
{
    item.hasParentSwitch = false;
    item.inHandSpace = HoldsWeapon(sequence);
    if (!g_pluginSettings.fixWeaponParentSwitch || !sequence.m_spTextKeys || !MovesWeapon(sequence))
        return;
    auto* transform = item.interpolator ? NI_DYNAMIC_CAST(NiTransformInterpolator, item.interpolator.data) : nullptr;
    if (!transform)
        return;

    std::optional<bool> bToRightHand;
    float fTextKey = 0.0f;
    for (const NiTextKey& key : sequence.m_spTextKeys->GetKeys())
    {
        const char* text = key.m_kText.CStr();
        if (text && (bToRightHand = ParseSwitchKey(text)))
        {
            fTextKey = key.m_fTime;
            break;
        }
    }
    if (!bToRightHand)
        return;

    TrackSwitch trackSwitch;
    unsigned int numKeys;
    NiAnimationKey::KeyType keyType;
    unsigned char keySize;
    if (auto* posData = transform->GetPosData(numKeys, keyType, keySize))
    {
        AddTrack(numKeys, [&](unsigned int i) { return posData->GetKeyAt(i, keySize)->m_fTime; },
            [&](unsigned int i) { return (posData->GetKeyAt(i, keySize)->GetPos() - posData->GetKeyAt(i - 1, keySize)->GetPos()).Length(); },
            fTextKey, trackSwitch);
    }
    if (auto* rotData = transform->GetRotData(numKeys, keyType, keySize); rotData && numKeys)
    {
        if (keyType == NiAnimationKey::EULERKEY)
        {
            // An Euler key's own time is never set. Its three float tracks hold the timing.
            auto* euler = static_cast<NiEulerRotKey*>(rotData->GetKeyAt(0, keySize));
            for (unsigned int axis = 0; axis < 3; ++axis)
            {
                auto getKey = [&](unsigned int i) { return euler->m_apkKeys[axis]->GetKeyAt(i, euler->m_ucSizes[axis]); };
                AddTrack(euler->m_uiNumKeys[axis], [&](unsigned int i) { return getKey(i)->m_fTime; },
                    [&](unsigned int i) { return std::abs(getKey(i)->GetValue() - getKey(i - 1)->GetValue()); },
                    fTextKey, trackSwitch);
            }
        }
        else
        {
            AddTrack(numKeys, [&](unsigned int i) { return rotData->GetKeyAt(i, keySize)->m_fTime; },
                [&](unsigned int i) { return RotationStep(rotData->GetKeyAt(i, keySize)->GetQuaternion(), rotData->GetKeyAt(i - 1, keySize)->GetQuaternion()); },
                fTextKey, trackSwitch);
        }
    }
    if (trackSwitch.fLastOldKey == INFINITY)
        return;
    item.parentSwitch = Window{ trackSwitch.fLastOldKey, trackSwitch.fSwitchKey, *bToRightHand };
    item.hasParentSwitch = true;
}

float WeaponParentSwitch::GetUpdateTime(const kBlendInterpItem* item, const NiObjectNET* weapon, float fTime)
{
    if (!item || !item->hasParentSwitch)
        return fTime;
    const auto& window = item->parentSwitch;
    // The engine reparents the node at its own time for the key, which kNVSE moves for respectEndKey animations, so go
    // by the parent the node has, not by the time
    const auto* parent = static_cast<const NiAVObject*>(weapon)->m_pkParent;
    const bool bOnRightHand = parent && parent->m_pcName.data == RightHandName().data;
    if (bOnRightHand != window.bToRightHand)
        return (std::min)(fTime, window.fLastOldKey); // still on the old parent
    return (std::max)(fTime, window.fSwitchKey);
}

bool WeaponParentSwitch::Counts(const kBlendInterpItem* item, const NiObjectNET* target)
{
    if (!item || !item->inHandSpace || !g_pluginSettings.fixWeaponParentSwitch)
        return true;
    const auto* parent = static_cast<const NiAVObject*>(target)->m_pkParent;
    return !parent || parent->m_pcName.data == RightHandName().data;
}

bool WeaponParentSwitch::EaseHolster(kBlendInterpolatorExtraData& extraData, NiObjectNET* target, float fDeltaTime, NiQuatTransform& kValue)
{
    if (!g_pluginSettings.easeWeaponHolster || !WeaponHandBlend::IsWeaponNode(target))
        return false;
    auto* weapon = static_cast<NiAVObject*>(target);
    auto& state = extraData.parentSwitch;
    if (!state)
        state = std::make_unique<NodeState>();

    const NiNode* parent = weapon->m_pkParent;
    const bool bWasOnRightHand = state->bOnRightHand;
    const bool bMoved = parent != state->lastParent;
    state->lastParent = parent;
    state->bOnRightHand = parent && parent->m_pcName.data == RightHandName().data;

    const bool bValid = kValue.IsTranslateValid() && kValue.IsRotateValid();
    // Nothing left in the blend in the middle of the ease: the unequip ended within it (its Detach and end a frame
    // apart) and the animations holding the weapon are left out (Counts). The node would stay where the ease had it.
    const bool bFromLast = state->bEasing && !bMoved && !kValue.IsTranslateValid() && !kValue.IsRotateValid();
    // UpdateLODBonesHook snaps bones that bone LOD skipped: their world transforms predate the skip and the unequip is over
    if ((!bValid && !bFromLast) || extraData.unsmoothedUpdate)
    {
        state->bEasing = false;
        return false;
    }
    if (bFromLast)
    {
        kValue.SetTranslate(state->lastTranslate);
        kValue.SetRotate(state->lastRotate);
    }
    if (bMoved)
    {
        state->bEasing = false;
        // The world transforms are those of the last evaluation, when the weapon was still on the hand. Not valid if
        // that was long ago (bone LOD, actor not updated).
        if (!bWasOnRightHand || !parent || state->bOnRightHand || fDeltaTime >= kHolsterEaseTime || !HasDetachItem(extraData))
            return false;
        const NiTransform& parentWorld = parent->m_kWorld;
        const NiTransform& weaponWorld = weapon->m_kWorld;
        const NiQuaternion parentRotate = UnitRotation(parentWorld.m_Rotate);
        const NiQuaternion inParentRotate = parentRotate.Inverse() * UnitRotation(weaponWorld.m_Rotate);
        NiPoint3 inParentTranslate;
        const NiPoint3 offset = weaponWorld.m_Translate - parentWorld.m_Translate;
        // The parent's rotation transposed, applied to the offset
        for (int axis = 0; axis < 3; ++axis)
        {
            const float fComponent = parentWorld.m_Rotate.m_pEntry[0][axis] * offset.x + parentWorld.m_Rotate.m_pEntry[1][axis] * offset.y +
                parentWorld.m_Rotate.m_pEntry[2][axis] * offset.z;
            (&inParentTranslate.x)[axis] = fComponent / parentWorld.m_fScale;
        }
        NiQuaternion blendRotate = kValue.GetRotate();
        blendRotate.Normalize();
        NiQuaternion rotateOffset = inParentRotate * blendRotate.Inverse();
        // Take the short way round (NiQuaternion::Slerp expects a non-negative dot product with the identity)
        if (rotateOffset.m_fW < 0.0f)
            rotateOffset = -rotateOffset;
        state->translateOffset = inParentTranslate - kValue.GetTranslate();
        state->rotateOffset = rotateOffset;
        state->fElapsed = 0.0f;
        state->bEasing = true;
    }
    else if (state->bEasing)
    {
        state->fElapsed += fDeltaTime;
    }
    if (!state->bEasing)
        return false;
    state->lastTranslate = kValue.GetTranslate();
    state->lastRotate = kValue.GetRotate();
    const float fProgress = state->fElapsed / kHolsterEaseTime;
    if (fProgress >= 1.0f)
    {
        state->bEasing = false;
        return bFromLast;
    }
    // Ease-out: the weapon leaves where the hand let it go and settles into its holster
    const float fRemaining = (1.0f - fProgress) * (1.0f - fProgress) * (1.0f - fProgress);
    kValue.SetTranslate(kValue.GetTranslate() + state->translateOffset * fRemaining);
    NiQuaternion rotate = NiQuaternion::Slerp(fRemaining, NiQuaternion::IDENTITY, state->rotateOffset) * kValue.GetRotate();
    rotate.Normalize();
    kValue.SetRotate(rotate);
    return bFromLast;
}
