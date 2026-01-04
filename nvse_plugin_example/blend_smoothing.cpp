#include "blend_smoothing.h"

#include <unordered_set>

#include "additive_anims.h"
#include "GameAPI.h"
#include "GameRTTI.h"
#include "hooks.h"
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
    if (this->poseInterp)
        this->poseInterp->DecrementRefCount();
    ThisStdCall(0xA7B300, this);
    if ((freeMem & 1) != 0)
        NiDelete(this);
}

bool kBlendInterpolatorExtraData::IsEqualEx(const kBlendInterpolatorExtraData* other) const
{
    return false;
}

void kBlendInterpolatorExtraData::EraseSequence(NiControllerSequence* anim)
{
    auto* owner = anim->GetOwner();
    if (!owner)
        return;
    auto* palette = owner->m_spObjectPalette;
    if (!palette)
        return;
    for (auto& block : anim->GetControlledBlocks())
    {
        auto* blendInterp = block.m_pkBlendInterp;
        if (!blendInterp)
            continue;
        auto& idTag = block.GetIDTag(anim);
        if (!idTag.m_kAVObjectName.data)
            continue;
        auto* target = palette->m_kHash.Lookup(idTag.m_kAVObjectName);
        if (!target)
            continue;
        auto* extraData = GetExtraData(target);
        if (!extraData)
            continue;
        for (auto& item : extraData->items)
        {
            if (item.sequence == anim)
                item.ClearValues();
        }
    }
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

kBlendInterpItem& kBlendInterpolatorExtraData::ObtainItem(NiInterpolator* interpolator)
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

NiInterpolator* CreatePoseInterpolator(NiAVObject* object)
{
    return NiTransformInterpolator::Create(object->m_kLocal);
}

kBlendInterpItem& kBlendInterpolatorExtraData::CreatePoseInterpItem(NiBlendInterpolator* blendInterp,
                                                                    NiControllerSequence* sequence,
                                                                    NiAVObject* target)
{
    auto* poseInterp = CreatePoseInterpolator(target);
    const auto poseIndex = blendInterp->AddInterpInfo(poseInterp, 1.0f, 0, 1.0f);
    DebugAssert(poseIndex != INVALID_INDEX);
    auto& poseItem = ObtainItem(poseInterp);
    poseItem.state = kInterpState::Activating;
    poseItem.sequence = sequence;
    poseItem.blendInterp = blendInterp;
    poseItem.isPoseInterp = true;
    poseItem.poseInterpIndex = poseIndex;
    return poseItem;
}

kBlendInterpItem* kBlendInterpolatorExtraData::GetItem(NiInterpolator* interpolator)
{
    for (auto& item : items)
    {
        if (item.interpolator == interpolator)
            return &item;
    }
    return nullptr;
}

kBlendInterpItem* kBlendInterpolatorExtraData::GetPoseInterpItem()
{
    for (auto& item : items)
    {
        if (item.isPoseInterp)
        {
            DebugAssert(item.poseInterpIndex != INVALID_INDEX);
            return &item;
        }
    }
    return nullptr;
}

NiTransformInterpolator *kBlendInterpolatorExtraData::ObtainPoseInterp(NiAVObject* target)
{
    if (!poseInterp)
        poseInterp = NiTransformInterpolator::Create(target->m_kLocal);
    return poseInterp;
}

void BlendSmoothing::ApplyForItems(kBlendInterpolatorExtraData* extraData,
    const std::vector<NiBlendInterpolator::InterpArrayItem*>& items, kWeightType type)
{
    if (!g_pluginSettings.blendSmoothing || !extraData || extraData->noBlendSmoothRequesterCount || items.empty())
        return;
    
    const auto deltaTime = g_timeGlobal->secondsPassed;
    const auto smoothingTime = g_pluginSettings.blendSmoothingRate;
    const auto smoothingRate = 1.0f - std::exp(-deltaTime / smoothingTime);
    
    constexpr float MIN_WEIGHT = 0.001f;

    // First pass: compute smoothed weights and accumulate sum
    float totalSmoothedWeight = 0.0f;
    
    for (auto* itemPtr : items)
    {
        auto& item = *itemPtr;
        if (!item.m_spInterpolator)
            continue;
            
        auto* extraItemPtr = extraData->GetItem(item.m_spInterpolator);
        if (!extraItemPtr || extraItemPtr->isAdditive)
            continue;
            
        auto& extraItem = *extraItemPtr;
        if (extraItem.debugState == kInterpDebugState::NotSet)
            continue;
            
        auto& weightState = *extraItem.GetWeightState(type);
        
        // Initialize on first frame
        if (weightState.lastSmoothedWeight == -NI_INFINITY)
        {
            weightState.lastSmoothedWeight = item.m_fNormalizedWeight;
        }
        
        float targetWeight = item.m_fNormalizedWeight;
        weightState.lastCalculatedNormalizedWeight = weightState.calculatedNormalizedWeight;
        weightState.calculatedNormalizedWeight = targetWeight;
        
        if (extraItem.detached)
            targetWeight = 0.0f;
        
        float smoothedWeight = std::lerp(weightState.lastSmoothedWeight, targetWeight, smoothingRate);
        
        // Snap to zero if below threshold (but don't normalize yet)
        if (smoothedWeight < MIN_WEIGHT)
            smoothedWeight = 0.0f;
        
        // Store temporarily in lastSmoothedWeight
        weightState.lastSmoothedWeight = smoothedWeight;
        totalSmoothedWeight += smoothedWeight;
    }

    // Second pass: renormalize to ensure sum = 1.0
    if (totalSmoothedWeight > 0.0f)
    {
        const float normalizationFactor = 1.0f / totalSmoothedWeight;
        
        for (auto* itemPtr : items)
        {
            auto& item = *itemPtr;
            if (!item.m_spInterpolator)
                continue;
                
            auto* extraItemPtr = extraData->GetItem(item.m_spInterpolator);
            if (!extraItemPtr || extraItemPtr->isAdditive)
                continue;
                
            auto& extraItem = *extraItemPtr;
            if (extraItem.debugState == kInterpDebugState::NotSet)
                continue;
                
            auto& weightState = *extraItem.GetWeightState(type);
            
            weightState.lastSmoothedWeight *= normalizationFactor;
            item.m_fNormalizedWeight = weightState.lastSmoothedWeight;
        }
    }
    else
    {
        DebugAssert(false); // All weights equal zero
        bool setFirst = false;
        for (auto* itemPtr : items)
        {
            if (!itemPtr->m_spInterpolator)
                continue;
            if (!setFirst)
            {
                itemPtr->m_fNormalizedWeight = 1.0f;
                setFirst = true;
                continue;
            }
            itemPtr->m_fNormalizedWeight = 0.0f;
        }
    }
}

void BlendSmoothing::DetachZeroWeightItems(kBlendInterpolatorExtraData* extraData, NiBlendInterpolator* blendInterp)
{
    if (!extraData || !g_pluginSettings.blendSmoothing)
        return;
    auto blendInterpItems = blendInterp->GetItems();
    for (auto& item : blendInterpItems)
    {
        auto* extraItemPtr = extraData->GetItem(item.m_spInterpolator);
        if (!extraItemPtr || extraItemPtr->isAdditive)
            continue;
        auto& extraItem = *extraItemPtr;
        if (extraItem.IsSmoothedWeightZero() && extraItem.state == kInterpState::Deactivating)
        {
            if (extraItem.detached)
            {
                const auto ucIndex = item.GetIndex(blendInterp);
                blendInterp->RemoveInterpInfo(ucIndex);
                extraItem.ClearValues();
                extraItem.debugState = kInterpDebugState::RemovedInBlendSmoothing;

                if (blendInterp->m_ucInterpCount == 1 && g_pluginSettings.poseInterpolators)
                {
                    if (auto* poseItem = extraData->GetPoseInterpItem())
                    {
                        DebugAssert(poseItem->poseInterpIndex < blendInterp->m_ucArraySize);
                        blendInterp->RemoveInterpInfo(poseItem->poseInterpIndex);
                        poseItem->ClearValues();
                    }
                }
            }
        }
    }
}

void BlendSmoothing::WriteHooks()
{
#if 1

    WriteRelCall(0x499252, INLINE_HOOK(NiControllerManager*, __fastcall, NiControllerManager** managerPtr)
    {
        auto* manager = *managerPtr;
        if (!manager)
            return nullptr;

        std::unordered_set<const char*> toKeep;
        std::unordered_map<const char*, NiAVObject*> toKeepMap;
        manager->m_spObjectPalette->m_pkScene->GetAsNiNode()->RecurseTree([&](NiAVObject* node)
        {
            toKeepMap.emplace(node->m_pcName, node);
            NiTFixedStringMap<NiAVObject*>::NiTMapItem* mapItem;
            if (manager->m_spObjectPalette->m_kHash.GetAt(node->m_pcName, mapItem))
            {
                toKeep.insert(node->m_pcName);
                if (mapItem->m_val != node)
                {
                    mapItem->m_val = node;
                }
            }
            if (auto* extraData = kBlendInterpolatorExtraData::GetExtraData(node))
            {
                auto& items = extraData->items;
                for (auto& item : items)
                {
                    item.ClearValues();
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
        for (auto& name : toRemove)
        {
            manager->m_spObjectPalette->m_kHash.RemoveAt(name);
        }
        return manager;
    }));

    static UInt32 uiNiControllerManagerDestroy = 0xA2EBB0;
    // clear object palette in NiControllerManager destructor
    WriteRelCall(0xA2EF83, INLINE_HOOK(void, __fastcall, NiControllerManager* manager)
    {
        manager->m_spObjectPalette->m_kHash.RemoveAll();
        ThisStdCall(uiNiControllerManagerDestroy, manager);
    }), &uiNiControllerManagerDestroy);


#endif
}
