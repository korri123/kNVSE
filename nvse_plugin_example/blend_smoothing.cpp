#include "blend_smoothing.h"

#include <unordered_set>

#include "additive_anims.h"
#include "GameAPI.h"
#include "hooks.h"
#include "SafeWrite.h"
#include "sequence_extradata.h"
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
    auto* palette = owner->GetObjectPalette();
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

kBlendInterpolatorExtraData* kBlendInterpolatorExtraData::GetOrCreate(NiObjectNET* obj)
{
    if (auto* extraData = GetExtraData(obj))
        return extraData;
    auto* extraData = Create();
    obj->AddExtraData(extraData);
    extraData->target = static_cast<NiAVObject*>(obj);
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

kBlendInterpItem& kBlendInterpolatorExtraData::GetOrCreateItem(NiInterpolator* interpolator)
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
    auto& poseItem = GetOrCreateItem(poseInterp);
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
        if (!extraItemPtr)
            continue;
        
        if (extraItemPtr->isAdditive)
        {
            DebugAssert(false);
            continue;
        }
            
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
        if (!extraItem.detached)
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
            if (!extraItemPtr)
                continue;
            
            if (extraItemPtr->isAdditive)
            {
                DebugAssert(false);
                continue;
            }
                
            auto& extraItem = *extraItemPtr;
            if (extraItem.debugState == kInterpDebugState::NotSet)
                continue;
                
            auto& weightState = *extraItem.GetWeightState(type);
            
            if (!extraItem.detached)
                weightState.lastSmoothedWeight *= normalizationFactor;
            item.m_fNormalizedWeight = weightState.lastSmoothedWeight;
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

namespace
{
    thread_local std::vector<NiControllerSequence*> g_activeSequences;
    void __fastcall HandleReloadTargets(NiControllerManager* manager)
    {
        if (manager->m_spObjectPalette)
        {
            for (const auto& item : manager->m_spObjectPalette->m_kHash)
            {
                auto* target = item.m_val;
                if (!target)
                    continue;
                auto* extraData = kBlendInterpolatorExtraData::GetExtraData(target);
                if (!extraData)
                    continue;
                extraData->ClearValues();
            }
        }
        
        g_activeSequences.clear();
        for (auto* sequence : manager->m_kActiveSequences)
        {
            if (auto* extraData = SequenceExtraDatas::Get(sequence); extraData && extraData->startInReloadTargets)
                g_activeSequences.emplace_back(sequence);
        }
        
        manager->DeactivateAll();
    }
    
    void __fastcall HandleSkipNextBlend(AnimData* animData)
    {
        animData->noBlend120 = true;
        animData->ReloadTargets();
    }
    
    void ReactivateSequencesHook()
    {
        auto* animData = GET_CALLER_VAR(AnimData*, -0x2C);
        auto* idx = GET_CALLER_VAR_PTR(UInt32*, -0x28);
        for (auto* sequence : g_activeSequences)
        {
            if (sequence->m_eState == NiControllerSequence::INACTIVE)
                animData->controllerManager->ActivateSequence(sequence, 0, false, 1.0f, 0.0f, nullptr);
        }
        g_activeSequences.clear();
        
        *idx = 0; // mov     [ebp+var_28], 0
        *static_cast<UInt32*>(_AddressOfReturnAddress()) = 0x499493;
    }
}

void BlendSmoothing::WriteHooks()
{

    // Animation::ReloadTargets
    WriteRelCall(0x499322, HandleReloadTargets);
    WriteRelCall(0x499481, ReactivateSequencesHook);
    PatchMemoryNop(0x499481 + 5, 4);

    // Animation::SkipNextBlend
    // noticed an issue that ReloadTargets wasn't being called from load game, so that means lingering extra data
    WriteRelCall(0x49B1A7, HandleSkipNextBlend);
    
}
