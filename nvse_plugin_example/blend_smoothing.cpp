#include "blend_smoothing.h"

#include "additive_anims.h"
#include "GameAPI.h"
#include "SafeWrite.h"

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
    for (auto i = 0u; i < obj->m_usExtraDataSize; i++) {
        NiExtraData* pData = obj->m_ppkExtra[i];
        if (pData && reinterpret_cast<DWORD*>(pData)[0] == reinterpret_cast<DWORD>(g_vtblExtraData))
            return static_cast<kBlendInterpolatorExtraData*>(pData);
    }
    auto* extraData = Create();
    obj->AddExtraData(extraData);
    return extraData;
}

kBlendInterpItem& kBlendInterpolatorExtraData::GetItem(NiInterpolator* interpolator)
{
    kBlendInterpItem* freeItem = nullptr;
    for (auto& item : items) {
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

#if _DEBUG
    struct DebugData
    {
        NiFixedString seqName;
        float fNormalizedWeight;
        float lastSmoothedWeight;
        float fEaseSpinner;
        float fWeight;
    };

    std::vector<DebugData> debugData;
    if (IsDebuggerPresent())
    {
        for (auto& item : blendInterpItems)
        {
            if (!item.m_spInterpolator)
                continue;
            const auto& extraItem = extraData->GetItem(item.m_spInterpolator);
            debugData.push_back(DebugData{
                .seqName = extraItem.sequence ? extraItem.sequence->m_kName : NiFixedString(""),
                .fNormalizedWeight = item.m_fNormalizedWeight,
                .lastSmoothedWeight = extraItem.lastSmoothedWeight,
                .fEaseSpinner = item.m_fEaseSpinner,
                .fWeight = item.m_fWeight
            });
        }
    }
    
#endif
    
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
        if (extraItem.detached)
            targetWeight = 0.0f;
        const auto smoothedWeight = std::lerp(extraItem.lastSmoothedWeight, targetWeight, smoothingRate);

        if (smoothedWeight < MIN_WEIGHT)
        {
            extraItem.lastSmoothedWeight = 0.0f;
            item.m_fNormalizedWeight = 0.0f;
            if (extraItem.detached)
            {
                blendInterp->RemoveInterpInfo(item.GetIndex(blendInterp));
                extraItem.ClearValues();
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
    
    DebugAssert(newTotalWeight == 0.0f || std::abs(newTotalWeight - 1.0f) < 0.001f);
}
