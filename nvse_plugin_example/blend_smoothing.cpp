#include "blend_smoothing.h"

#include <unordered_set>

#include "additive_anims.h"
#include "GameAPI.h"
#include "GameRTTI.h"
#include "SafeWrite.h"
#include "utility.h"

void* g_vtblExtraData[37] = {};

void PopulateVtable(kBlendInterpolatorExtraData* extraData)
{
    if (!g_vtblExtraData[0])
    {
        for (auto i = 0; i < std::size(g_vtblExtraData); i++) {
            g_vtblExtraData[i] = reinterpret_cast<void***>(extraData)[0][i];
        }
        ReplaceVTableEntry(g_vtblExtraData, 0, &kBlendInterpolatorExtraData::Destroy);
        ReplaceVTableEntry(g_vtblExtraData, 2, &kBlendInterpolatorExtraData::GetRTTIEx);
        ReplaceVTableEntry(g_vtblExtraData, 23, &kBlendInterpolatorExtraData::IsEqualEx);
    }
    reinterpret_cast<UInt32*>(extraData)[0] = reinterpret_cast<UInt32>(g_vtblExtraData);
}

void kBlendInterpolatorExtraData::Destroy(bool freeMem)
{
    this->items.~vector();
    ThisStdCall(0xA7B300, this);
    if ((freeMem & 1) != 0)
        NiDelete(this);
}

bool kBlendInterpolatorExtraData::IsEqualEx(const kBlendInterpolatorExtraData* other) const
{
    return false;
}

kBlendInterpolatorExtraData* kBlendInterpolatorExtraData::Create()
{
    auto* extraData = NiNew<kBlendInterpolatorExtraData>();
    ThisStdCall(0xA7B2E0, extraData); // ctor
    new(&extraData->items) std::vector<kBlendInterpItem>();
    PopulateVtable(extraData);
    extraData->m_kName = GetKey();
    return extraData;
}

const NiFixedString& kBlendInterpolatorExtraData::GetKey()
{
    const static auto name = NiFixedString("kBlendInterpolatorExtraData");
    return name;
}

kBlendInterpolatorExtraData* kBlendInterpolatorExtraData::Obtain(NiObjectNET* obj)
{
    if (auto* extraData = GetExtraData(obj))
        return extraData;
    auto* extraData = Create();
    obj->AddExtraData(extraData);
    return extraData;
}

kBlendInterpolatorExtraData* kBlendInterpolatorExtraData::GetExtraData(NiObjectNET* obj)
{
    for (auto i = 0u; i < obj->m_usExtraDataSize; i++)
    {
        NiExtraData* pData = obj->m_ppkExtra[i];
        if (pData && reinterpret_cast<DWORD*>(pData)[0] == reinterpret_cast<DWORD>(g_vtblExtraData))
            return static_cast<kBlendInterpolatorExtraData*>(pData);
    }
    return nullptr;
}

kBlendInterpItem& kBlendInterpolatorExtraData::GetItem(NiInterpolator* interpolator)
{
    kBlendInterpItem* freeItem = nullptr;
    for (auto& item : items)
    {
        if (item.interpolator == interpolator)
            return item;
        if (!item.interpolator && !freeItem)
            freeItem = &item;
    }
    if (freeItem)
    {
        freeItem->interpolator = interpolator;
        return *freeItem;
    }
    return items.emplace_back(interpolator);
}

void BlendSmoothing::Apply(NiBlendInterpolator* blendInterp, NiObjectNET* target)
{
    const auto deltaTime = g_timeGlobal->secondsPassed;
    constexpr auto smoothingTime = 0.1f;
    const auto smoothingRate = 1.0f - std::exp(-deltaTime / smoothingTime);
    constexpr float MIN_WEIGHT = 0.001f;
    
    float totalWeight = 0.0f;
    auto* extraData = kBlendInterpolatorExtraData::Obtain(target);

    auto blendInterpItems = blendInterp->GetItems();
    
    for (auto& item : blendInterpItems)
    {
        if (!item.m_spInterpolator)
            continue;
        auto& extraItem = extraData->GetItem(item.m_spInterpolator);
        if (extraItem.lastSmoothedWeight == -NI_INFINITY)
        {
            extraItem.lastSmoothedWeight = item.m_fNormalizedWeight;
            totalWeight += item.m_fNormalizedWeight;
            continue;
        }
        
        auto targetWeight = item.m_fNormalizedWeight;
        extraItem.lastCalculatedNormalizedWeight = extraItem.calculatedNormalizedWeight;
        extraItem.calculatedNormalizedWeight = targetWeight;
        if (extraItem.detached)
            targetWeight = 0.0f;
        const auto smoothedWeight = std::lerp(extraItem.lastSmoothedWeight, targetWeight, smoothingRate);

        if (smoothedWeight < MIN_WEIGHT)
        {
            extraItem.lastSmoothedWeight = 0.0f;
            item.m_fNormalizedWeight = 0.0f;
            if (extraItem.detached)
            {
                const auto ucIndex = item.GetIndex(blendInterp);
                blendInterp->RemoveInterpInfo(ucIndex);
                extraItem.ClearValues();
                extraItem.state = kInterpState::RemovedInBlendSmoothing;
            }
            continue;
        }
        
        extraItem.lastSmoothedWeight = smoothedWeight;
        item.m_fNormalizedWeight = smoothedWeight;

        if (smoothedWeight > 0.999f) 
        {
            extraItem.lastSmoothedWeight = 1.0f;
            item.m_fNormalizedWeight = 1.0f;
        }
        totalWeight += item.m_fNormalizedWeight;
    }

#if 0
    float newTotalWeight = 0.0f;
	
    if (totalWeight > 0.0f)
    {
        // renormalize weights
        const auto invTotalWeight = 1.0f / totalWeight;
        for (auto& item : blendInterpItems)
        {
            if (!item.m_spInterpolator || AdditiveManager::IsAdditiveInterpolator(item.m_spInterpolator))
                continue;
            item.m_fNormalizedWeight *= invTotalWeight;
            newTotalWeight += item.m_fNormalizedWeight;
        }
    }

#if _DEBUG
    for (auto& item : blendInterpItems)
    {
        if (!item.m_spInterpolator)
            continue;
        auto& extraItem = extraData->GetItem(item.m_spInterpolator);
        if (extraItem.lastNormalizedWeight == -NI_INFINITY)
        {
            extraItem.lastNormalizedWeight = item.m_fNormalizedWeight;
            continue;
        }
        DebugAssert(std::abs(extraItem.lastNormalizedWeight - item.m_fNormalizedWeight) < 0.5f);
        extraItem.lastNormalizedWeight = item.m_fNormalizedWeight;
    }
#endif
    
    DebugAssert(newTotalWeight == 0.0f || std::abs(newTotalWeight - 1.0f) < 0.001f);
#endif
}

void ClearExtraDataInterpItems(NiMultiTargetTransformController* pController)
{
    const auto targets = pController->GetTargets();
    for (auto* target : targets)
    {
        if (!target)
            continue;
        if (auto* extraData = kBlendInterpolatorExtraData::GetExtraData(target))
        {
            for (auto& item : extraData->items)
            {
                item.ClearValues();
                item.state = kInterpState::InterpControllerDestroyed;
            }
        }
    }
}

void DecreateTargetRefCounts(NiMultiTargetTransformController* pController)
{
    for (auto* item : pController->GetTargets())
    {
        if (item)
            item->DecrementRefCount();
    }
}

void __fastcall HandleAssignTarget(const UInt16 index, NiAVObject** targets, NiAVObject* target)
{
    auto& replaceTarget = targets[index];
    if (replaceTarget == target)
        return;
    if (replaceTarget)
        replaceTarget->DecrementRefCount();
    target->IncrementRefCount();
    targets[index] = target;
}

__declspec(naked) void AddTargetHook1()
{
    __asm
    {
        mov edx, edi
        push esi
        movzx eax, cx
        mov ecx, eax
        call HandleAssignTarget
        mov eax, 0xA31E1A
        jmp eax
    }
}

__declspec(naked) void AddTargetHook2()
{
    __asm
    {
        push ecx
        push eax
        push esi
        movzx eax, cx
        mov ecx, eax
        call HandleAssignTarget
        pop eax
        pop ecx
        mov edx, 0xA31DFB
        jmp edx
    }
}


void BlendSmoothing::WriteHooks()
{

#if 1

    WriteRelCall(0x499252, INLINE_HOOK(NiControllerManager*, __fastcall, NiControllerManager** managerPtr)
    {
        auto* manager = *managerPtr;
        int iCount = 0;

        std::unordered_set<const char*> toKeep;
        std::unordered_map<const char*, NiAVObject*> toKeepMap;
        manager->m_spObjectPalette->m_pkScene->GetAsNiNode()->RecurseTree([&](NiAVObject* node)
        {
            toKeepMap.emplace(node->m_pcName, node);
            NiTFixedStringMap<NiAVObject*>::NiTMapItem* item;
            if (manager->m_spObjectPalette->m_kHash.GetAt(node->m_pcName, item))
            {
                toKeep.insert(node->m_pcName);
                if (item->m_val != node)
                {
                    item->m_val = node;
                }
            }
        });
        std::vector<const char*> toRemove;
        for (auto& mapItem : manager->m_spObjectPalette->m_kHash)
        {
            auto& name = mapItem.m_key;
            if (!toKeep.contains(name))
                toRemove.emplace_back(name);
        }
        UInt32 numRemoved = 0;
        for (auto& name : toRemove)
        {
            ++numRemoved;
            manager->m_spObjectPalette->m_kHash.RemoveAt(name);
        }
        
        for (auto& mapItem : manager->m_spObjectPalette->m_kHash)
        {
            ++iCount;
            auto* node = mapItem.m_val;
            if (!node)
                continue;
            if (auto* extraData = kBlendInterpolatorExtraData::GetExtraData(node))
            {
                auto& items = extraData->items;
                for (auto& item : items)
                {
                    item.ClearValues();
                }
            }
        }
        DebugAssert(iCount == manager->m_spObjectPalette->m_kHash.m_uiCount);
        return manager;
    }));
    
    WriteRelJump(0xA31E14, &AddTargetHook1);
    PatchMemoryNop(0xA31E14 + 5, 1);

    WriteRelJump(0xA31DF5, &AddTargetHook2);
    PatchMemoryNop(0xA31DF5 + 5, 1);
    
    static UInt32 uiNiMultiTargetTransformControllerDestroy = 0xA2FB70;
    WriteRelCall(0xA30283, INLINE_HOOK(void, __fastcall, NiMultiTargetTransformController* pController)
    {
        ClearExtraDataInterpItems(pController);
        DecreateTargetRefCounts(pController);
        ThisStdCall(uiNiMultiTargetTransformControllerDestroy, pController);
    }), &uiNiMultiTargetTransformControllerDestroy);

    static UInt32 uiNiControllerManagerDestroy = 0xA2EBB0;
    // clear object palette in NiControllerManager destructor
    WriteRelCall(0xA2EF83, INLINE_HOOK(void, __fastcall, NiControllerManager* manager)
    {
        manager->m_spObjectPalette->m_kHash.RemoveAll();
        ThisStdCall(uiNiControllerManagerDestroy, manager);
    }), &uiNiControllerManagerDestroy);

    static UInt32 uiNiStreamResolveLinkID = 0xA64620;

    // NiStream::ResolveLinkID
    WriteRelCall(0xA30422, INLINE_HOOK(NiAVObject*, __fastcall, NiStream* stream)
    {
        auto* result = ThisStdCall<NiAVObject*>(uiNiStreamResolveLinkID, stream);
        if (result)
            result->IncrementRefCount();
        return result;
    }), &uiNiStreamResolveLinkID);

    static UInt32 uiNiMultiTargetTransformControllerAddInterpolatorTarget = 0x43B320;
    
    // NiMultiTargetTransformController::AddInterpolatorTarget
    WriteRelCall(0x4F0678, INLINE_HOOK(void, __fastcall, NiMultiTargetTransformController* controller, void*, UInt16 index, NiAVObject* target)
    {
        if (target)
            target->IncrementRefCount();
        ThisStdCall(uiNiMultiTargetTransformControllerAddInterpolatorTarget, controller, index, target);
    }), &uiNiMultiTargetTransformControllerAddInterpolatorTarget);

#endif
}
