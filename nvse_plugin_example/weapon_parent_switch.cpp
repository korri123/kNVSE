#include "weapon_parent_switch.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

#include "blend_smoothing.h"
#include "hooks.h"
#include "NiObjects.h"
#include "string_view_util.h"

namespace
{
    // the text key and the Weapon track's first key relative to the new parent are both at the switch
    constexpr float kSameKeyTime = 1e-3f;

    const NiFixedString& RightHandName()
    {
        static const NiFixedString name("Bip01 R Hand");
        return name;
    }

    // A track switching parent space at fSwitchKey has a key there: the key before it is its last one relative to the old
    // parent. Tracks without a key at the switch are left alone
    template <typename GetTime>
    void AddTrack(unsigned int numKeys, GetTime&& getTime, float fSwitchKey, float& fLastOldKey)
    {
        for (unsigned int i = 1; i < numKeys; ++i)
        {
            if (std::abs(getTime(i) - fSwitchKey) <= kSameKeyTime)
            {
                fLastOldKey = (std::min)(fLastOldKey, getTime(i - 1));
                return;
            }
        }
    }

    // BSAnimGroupSequence::InitAnimGroup (0x5F3A20) reads a text key line by line and takes a line starting with the
    // action name, in any case (strnicmp over the name's length). Returns whether it is Attach (to the right hand)
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
    // their Attach/Detach key. A throw attack's Attach is where the next projectile appears in the hand the node never
    // left, and its Weapon track before that key is the throwing grip, in the same space
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
}

void WeaponParentSwitch::Init(kBlendInterpItem& item, const NiControllerSequence& sequence)
{
    item.hasParentSwitch = false;
    if (!g_pluginSettings.fixWeaponParentSwitch || !sequence.m_spTextKeys || !MovesWeapon(sequence))
        return;
    auto* transform = item.interpolator ? NI_DYNAMIC_CAST(NiTransformInterpolator, item.interpolator.data) : nullptr;
    if (!transform)
        return;

    std::optional<bool> bToRightHand;
    float fSwitchKey = 0.0f;
    for (const NiTextKey& key : sequence.m_spTextKeys->GetKeys())
    {
        const char* text = key.m_kText.CStr();
        if (text && (bToRightHand = ParseSwitchKey(text)))
        {
            fSwitchKey = key.m_fTime;
            break;
        }
    }
    if (!bToRightHand)
        return;

    float fLastOldKey = INFINITY;
    unsigned int numKeys;
    NiAnimationKey::KeyType keyType;
    unsigned char keySize;
    if (auto* posData = transform->GetPosData(numKeys, keyType, keySize))
        AddTrack(numKeys, [&](unsigned int i) { return posData->GetKeyAt(i, keySize)->m_fTime; }, fSwitchKey, fLastOldKey);
    if (auto* rotData = transform->GetRotData(numKeys, keyType, keySize); rotData && numKeys)
    {
        if (keyType == NiAnimationKey::EULERKEY)
        {
            // an Euler key's own time is never set; its three float tracks hold the timing
            auto* euler = static_cast<NiEulerRotKey*>(rotData->GetKeyAt(0, keySize));
            for (unsigned int axis = 0; axis < 3; ++axis)
                AddTrack(euler->m_uiNumKeys[axis], [&](unsigned int i) { return euler->m_apkKeys[axis]->GetKeyAt(i, euler->m_ucSizes[axis])->m_fTime; },
                    fSwitchKey, fLastOldKey);
        }
        else
        {
            AddTrack(numKeys, [&](unsigned int i) { return rotData->GetKeyAt(i, keySize)->m_fTime; }, fSwitchKey, fLastOldKey);
        }
    }
    if (fLastOldKey == INFINITY)
        return;
    item.parentSwitch = Window{ fLastOldKey, fSwitchKey, *bToRightHand };
    item.hasParentSwitch = true;
}

float WeaponParentSwitch::GetUpdateTime(const kBlendInterpItem* item, const NiObjectNET* weapon, float fTime)
{
    if (!item || !item->hasParentSwitch)
        return fTime;
    const auto& window = item->parentSwitch;
    // the engine reparents the node at its own time for the key, which kNVSE moves for respectEndKey animations: go by
    // the parent the node has, not by the time
    const auto* parent = static_cast<const NiAVObject*>(weapon)->m_pkParent;
    const bool bOnRightHand = parent && parent->m_pcName.data == RightHandName().data;
    if (bOnRightHand != window.bToRightHand)
        return (std::min)(fTime, window.fLastOldKey); // still on the old parent
    return (std::max)(fTime, window.fSwitchKey);
}
