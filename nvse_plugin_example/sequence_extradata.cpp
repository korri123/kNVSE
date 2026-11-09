#include "sequence_extradata.h"

#include <algorithm>

#include "GameAPI.h"
#include "SafeWrite.h"
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

    kAnimationExtraData::Entry* FindEntry(kAnimationExtraData* nodeData, const NiControllerSequence* sequence)
    {
        for (auto& entry : nodeData->entries)
            if (entry.sequence == sequence && entry.manager == sequence->m_pkOwner)
                return &entry;
        return nullptr;
    }

    // NiControllerManager::RemoveSequence and RemoveAllSequences clear a sequence's targets just before it leaves the
    // manager, while m_pkOwner still points at it. Both run on a live Animation whose manager is attached to its scene root:
    // ~Animation dereferences the manager's target itself right after RemoveAllSequences.
    void __fastcall ClearTargetsAndDelete(NiControllerSequence* sequence)
    {
        ThisStdCall(0xA30880, sequence); // NiControllerSequence::ClearTargets
        SequenceExtraDatas::Delete(sequence);
    }
}

void kAnimationExtraData::Destroy(bool freeMem)
{
    this->entries.~vector();
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
    new(&extraData->entries) std::vector<Entry>();
    PopulateVtable(extraData);
    extraData->m_kName = ExtraDataName();
    obj->AddExtraData(extraData);
    return extraData;
}

SequenceExtraData* SequenceExtraDatas::Get(const NiControllerSequence* sequence)
{
    auto* nodeData = GetNodeData(sequence);
    auto* entry = nodeData ? FindEntry(nodeData, sequence) : nullptr;
    return entry ? &entry->data : nullptr;
}

SequenceExtraData* SequenceExtraDatas::GetOrCreate(NiControllerSequence* sequence)
{
    NiControllerManager* manager = sequence->m_pkOwner;
    if (!manager || !manager->m_pkTarget)
        return nullptr;
    auto* nodeData = kAnimationExtraData::GetOrCreate(manager->m_pkTarget);
    if (auto* entry = FindEntry(nodeData, sequence))
        return &entry->data;
    return &nodeData->entries.emplace_back(manager, sequence).data;
}

void SequenceExtraDatas::Delete(const NiControllerSequence* sequence)
{
    auto* nodeData = GetNodeData(sequence);
    if (!nodeData)
        return;
    auto& entries = nodeData->entries;
    const auto iter = std::ranges::find_if(entries, [&](const kAnimationExtraData::Entry& entry)
    {
        return entry.sequence == sequence && entry.manager == sequence->m_pkOwner;
    });
    if (iter == entries.end())
        return;
    if (&*iter != &entries.back())
        *iter = std::move(entries.back());
    entries.pop_back();
}

void SequenceExtraDatas::WriteHooks()
{
    WriteRelCall(0xA2E933, ClearTargetsAndDelete); // NiControllerManager::RemoveSequence
    WriteRelCall(0xA2E9BC, ClearTargetsAndDelete); // NiControllerManager::RemoveAllSequences
}
