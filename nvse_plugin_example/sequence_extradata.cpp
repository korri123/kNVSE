#include "sequence_extradata.h"

#include <algorithm>
#include <cmath>

#include "blend_curves.h"
#include "GameAPI.h"
#include "SafeWrite.h"
#include "spawn_keys.h"
#include "utility.h"

namespace
{
    void* g_vtblAnimationExtraData[37] = {};

    void PopulateVtable(kAnimationExtraData* extraData)
    {
        if (!g_vtblAnimationExtraData[0])
        {
            for (auto i = 0; i < std::size(g_vtblAnimationExtraData); i++)
                g_vtblAnimationExtraData[i] = reinterpret_cast<void***>(extraData)[0][i];
            ReplaceVTableEntry(g_vtblAnimationExtraData, 0, &kAnimationExtraData::Destroy);
            ReplaceVTableEntry(g_vtblAnimationExtraData, 2, &kAnimationExtraData::GetRTTIEx);
            ReplaceVTableEntry(g_vtblAnimationExtraData, 23, &kAnimationExtraData::IsEqualEx);
        }
        reinterpret_cast<UInt32*>(extraData)[0] = reinterpret_cast<UInt32>(g_vtblAnimationExtraData);
    }

    const NiFixedString& ExtraDataName()
    {
        static const NiFixedString name("kAnimationExtraData");
        return name;
    }

    kAnimationExtraData* GetNodeData(const NiControllerSequence* sequence)
    {
        const NiControllerManager* manager = sequence->m_pkOwner;
        return manager && manager->m_pkTarget ? kAnimationExtraData::Get(manager->m_pkTarget) : nullptr;
    }

    SequenceExtraData* FindData(kAnimationExtraData* nodeData, const NiControllerSequence* sequence)
    {
        const kAnimationExtraData::SequenceKey key{ sequence->m_pkOwner, sequence };
        const auto& keys = nodeData->sequenceKeys;
        // Fibonacci hashing, because sequences are heap blocks whose addresses share their low bits
        UInt16& hint = nodeData->sequenceHints[reinterpret_cast<UInt32>(sequence) * 0x9E3779B1u >>
            (32 - kAnimationExtraData::kSequenceHintBits)];
        if (hint < keys.size() && keys[hint] == key)
            return &nodeData->sequenceData[hint];
        const auto iter = std::ranges::find(keys, key);
        if (iter == keys.end())
            return nullptr;
        const auto index = static_cast<size_t>(iter - keys.begin());
        hint = static_cast<UInt16>(index);
        return &nodeData->sequenceData[index];
    }

    // Hook for NiControllerManager::RemoveSequence and RemoveAllSequences: clears a sequence's targets just before it
    // leaves the manager, while m_pkOwner still points at it. Both run on a live Animation whose manager is attached to
    // its scene root (~Animation dereferences the manager's target itself right after RemoveAllSequences).
    void __fastcall ClearTargetsAndDelete(NiControllerSequence* sequence)
    {
        // Sequences following this one (NiControllerSequence::m_pkPartnerSequence) would read it after it is freed, so
        // release them first. This must happen before ClearTargets, which deactivates the sequence and rebases its
        // offset. If it ran on its own clock, the owner time of its last evaluation is the stored time minus the offset.
        const bool bOwnClock = sequence->m_fLastTime != -NI_INFINITY && !sequence->m_pkPartnerSequence &&
            sequence->m_fDestFrame == -NI_INFINITY;
        sequence->ReleaseFollowers(bOwnClock ? sequence->m_fLastTime - sequence->m_fOffset : NAN);
        ThisStdCall(0xA30880, sequence); // NiControllerSequence::ClearTargets
        SequenceExtraDatas::Delete(sequence);
    }

    // Hook for NiControllerManager::AddSequence binding the sequence it adds (NiControllerSequence::StoreTargets).
    // m_pkOwner is set and the sequence is not in the manager's array yet. Never reached for a null manager
    // (JohnnyGuitar's fix of the check at 0xA2F0CA returns before).
    bool __fastcall StoreTargetsOnJoin(NiControllerSequence* sequence, void*, NiAVObject* root)
    {
        SpawnKeys::OnSequenceJoin(*sequence);
        BlendCurves::OnSequenceJoin(*sequence);
        if (sequence->StoreTargets(root))
            return true;
        // AddSequence clears m_pkOwner and gives up
        SequenceExtraDatas::Delete(sequence);
        return false;
    }
}

void kAnimationExtraData::Destroy(bool freeMem)
{
    this->sequenceKeys.~vector();
    this->sequenceData.~vector();
    this->hardCuts.~vector();
    this->pendingStoreTargets.~vector();
    this->addedNodes.~vector();
    this->overrides.~ActorOverrides();
    ThisStdCall(0xA7B300, this); // ~NiExtraData
    if ((freeMem & 1) != 0)
        NiDelete(this);
}

bool kAnimationExtraData::IsEqualEx(const kAnimationExtraData* other) const
{
    return false;
}

kAnimationExtraData* kAnimationExtraData::Get(NiObjectNET* obj)
{
    for (auto i = 0u; i < obj->m_usExtraDataSize; i++)
    {
        NiExtraData* pData = obj->m_ppkExtra[i];
        if (pData && reinterpret_cast<DWORD*>(pData)[0] == reinterpret_cast<DWORD>(g_vtblAnimationExtraData))
            return static_cast<kAnimationExtraData*>(pData);
    }
    return nullptr;
}

kAnimationExtraData* kAnimationExtraData::GetOrCreate(NiObjectNET* obj)
{
    if (auto* extraData = Get(obj))
        return extraData;
    auto* extraData = NiNew<kAnimationExtraData>();
    ThisStdCall(0xA7B2E0, extraData); // NiExtraData::NiExtraData
    new(&extraData->sequenceKeys) std::vector<SequenceKey>();
    new(&extraData->sequenceData) std::vector<SequenceExtraData>();
    std::ranges::fill(extraData->sequenceHints, UInt16(0));
    new(&extraData->hardCuts) std::vector<HardCut>();
    new(&extraData->pendingStoreTargets) std::vector<SequenceKey>();
    new(&extraData->openSwitch) OpenSwitch();
    extraData->aimPitch = 0.0f;
    extraData->namedCurveEaseEnd = -NI_INFINITY;
    new(&extraData->addedNodes) std::vector<SkeletonNodes::Node>();
    new(&extraData->overrides) ActorOverrides();
    PopulateVtable(extraData);
    extraData->m_kName = ExtraDataName();
    obj->AddExtraData(extraData);
    return extraData;
}

SequenceExtraData* SequenceExtraDatas::Get(const NiControllerSequence* sequence)
{
    auto* nodeData = GetNodeData(sequence);
    return nodeData ? FindData(nodeData, sequence) : nullptr;
}

SequenceExtraData* SequenceExtraDatas::GetOrCreate(NiControllerSequence* sequence)
{
    NiControllerManager* manager = sequence->m_pkOwner;
    if (!manager || !manager->m_pkTarget)
        return nullptr;
    auto* nodeData = kAnimationExtraData::GetOrCreate(manager->m_pkTarget);
    if (auto* data = FindData(nodeData, sequence))
        return data;
    nodeData->sequenceKeys.push_back({ manager, sequence });
    return &nodeData->sequenceData.emplace_back();
}

void SequenceExtraDatas::Delete(const NiControllerSequence* sequence)
{
    auto* nodeData = GetNodeData(sequence);
    if (!nodeData)
        return;
    const NiControllerManager* manager = sequence->m_pkOwner;
    std::erase_if(nodeData->hardCuts, [&](const kAnimationExtraData::HardCut& cut)
    {
        return cut.sequence == sequence && cut.manager == manager;
    });
    std::erase(nodeData->pendingStoreTargets, kAnimationExtraData::SequenceKey{ manager, sequence });
    auto& keys = nodeData->sequenceKeys;
    auto& data = nodeData->sequenceData;
    const auto iter = std::ranges::find(keys, kAnimationExtraData::SequenceKey{ manager, sequence });
    if (iter == keys.end())
        return;
    // The last entry takes its place
    const auto index = static_cast<size_t>(iter - keys.begin());
    if (index != keys.size() - 1)
    {
        keys[index] = keys.back();
        data[index] = std::move(data.back());
    }
    keys.pop_back();
    data.pop_back();
}

void SequenceExtraDatas::MarkStoreTargets(NiControllerManager* manager)
{
    if (!manager->m_pkTarget)
        return;
    // Every sequence of the manager is marked. The manager's entries are replaced, which also drops duplicates. Called
    // on every update of a dying actor (TESObjectREFR::ClearInterpolators from Actor::Update).
    auto& pending = kAnimationExtraData::GetOrCreate(manager->m_pkTarget)->pendingStoreTargets;
    std::erase_if(pending, [manager](const kAnimationExtraData::SequenceKey& key) { return key.manager == manager; });
    pending.reserve(pending.size() + manager->sequences.Length());
    for (auto& sequence : manager->sequences)
    {
        if (sequence)
            pending.push_back({ manager, sequence });
    }
}

bool SequenceExtraDatas::TakeStoreTargets(const NiControllerSequence* sequence)
{
    auto* nodeData = GetNodeData(sequence);
    if (!nodeData || nodeData->pendingStoreTargets.empty())
        return false;
    auto& pending = nodeData->pendingStoreTargets;
    const auto iter = std::ranges::find(pending, kAnimationExtraData::SequenceKey{ sequence->m_pkOwner, sequence });
    if (iter == pending.end())
        return false;
    *iter = pending.back();
    pending.pop_back();
    return true;
}

void SequenceExtraDatas::WriteHooks()
{
    WriteRelCall(0xA2F0F6, StoreTargetsOnJoin); // NiControllerManager::AddSequence
    WriteRelCall(0xA2E933, ClearTargetsAndDelete); // NiControllerManager::RemoveSequence
    WriteRelCall(0xA2E9BC, ClearTargetsAndDelete); // NiControllerManager::RemoveAllSequences
}
