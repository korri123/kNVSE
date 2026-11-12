#include "blend_smoothing.h"

#include "additive_anims.h"
#include "GameAPI.h"
#include "hooks.h"
#include "SafeWrite.h"
#include "sequence_extradata.h"
#include "skeleton_nodes.h"
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
    this->weaponHandBlend.~unique_ptr();
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
    new(&extraData->weaponHandBlend) std::unique_ptr<WeaponHandBlend::NodeData>();
    // NiNew does not construct
    extraData->accumRootChild = false;
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

kBlendInterpItem* kBlendInterpolatorExtraData::GetItem(NiInterpolator* interpolator)
{
    for (auto& item : items)
    {
        if (item.interpolator == interpolator)
            return &item;
    }
    return nullptr;
}

float BlendSmoothing::GetElapsedTime(float fLastTime, float fTime)
{
    // not evaluated before, or the animation time went back: one frame
    if (fLastTime == -NI_INFINITY || fTime < fLastTime)
        return g_timeGlobal->secondsPassed;
    return fTime - fLastTime;
}

void BlendSmoothing::Step::AddItem(const NiBlendInterpolator::InterpArrayItem& item, kBlendInterpItem* extraItem,
    const NiQuatTransform& kTransform)
{
    // a detached item is only fading out: no sequence has it attached, nothing moves its weight or ease spinner
    if (extraItem && extraItem->detached)
        return;

    // the channels count too: each is blended from the items that have it. Summed, as the order of the items means nothing
    const UInt32 uiChannels = (kTransform.IsTranslateValid() ? 1 : 0) | (kTransform.IsRotateValid() ? 2 : 0)
        | (kTransform.IsScaleValid() ? 4 : 0);
    m_uiAttachedItems += (reinterpret_cast<UInt32>(item.m_spInterpolator.data) * 2654435761u)
        ^ (((static_cast<UInt8>(item.m_cPriority) << 3) | uiChannels) * 2246822519u);

    if (!extraItem)
        return;
    if (extraItem->lastWeight != -NI_INFINITY)
    {
        const float fChange = std::abs(item.m_fWeight - extraItem->lastWeight)
            + std::abs(item.m_fEaseSpinner - extraItem->lastEaseSpinner);
        fInputChange = max(fInputChange, fChange);
    }
    extraItem->lastWeight = item.m_fWeight;
    extraItem->lastEaseSpinner = item.m_fEaseSpinner;
}

// The children of the accum root (Bip01 NonAccum) are posed relative to the actor, which the engine turns at once when it
// sits down or stands up: the furniture enter and exit animations are made for the way the actor faces after the turn,
// which comes with the switch of sequences, without a blend. The actor's angle is set before the evaluation that blends
// the switch, its 3D is turned after it, before the frame is drawn. Whatever weight smoothing took up from that switch
// posed the node relative to the old facing, turned with the actor: the body flipped around and turned back over
// fBlendSmoothingRate.
static bool ActorTurnsAtOnce(const kBlendInterpolatorExtraData* extraData)
{
    NiAVObject* target = extraData->target;
    if (!extraData->accumRootChild || !target || !target->m_pkParent)
        return false;
    auto* actor = TESObjectREFR::FindReferenceFor3D(target->m_pkParent);
    if (!actor || !actor->IsActor())
        return false;
    // the facing of the 3D the node is posed in, against the actor's angle: turning while walking or in place stays well
    // below this per frame, the engine turns actors onto furniture by multiples of 90 degrees
    constexpr float MIN_COS_TURN = 0.70710678f; // 45 degrees
    const NiMatrix33& kRotate = target->m_pkParent->m_kWorld.m_Rotate;
    const float fForwardX = kRotate.m_pEntry[0][1];
    const float fForwardY = kRotate.m_pEntry[1][1];
    const float fLength = std::sqrt(fForwardX * fForwardX + fForwardY * fForwardY);
    return fLength > 0.5f
        && fForwardX * std::sin(actor->rotZ) + fForwardY * std::cos(actor->rotZ) < MIN_COS_TURN * fLength;
}

void BlendSmoothing::Step::End(kBlendInterpolatorExtraData* extraData)
{
    if (!extraData)
        return;
    bItemsChanged = extraData->attachedItems != m_uiAttachedItems;
    extraData->attachedItems = m_uiAttachedItems;

    // noSmoothing is asked for from attach to detach, whether or not the item is blended this time
    bDisabled = ra::any_of(extraData->items, [](const kBlendInterpItem& item) { return item.noSmoothing && !item.detached; })
        || ActorTurnsAtOnce(extraData);
    if (bDisabled)
    {
        // items still fading out are cut (DetachZeroWeightItems), and smoothing starts over once it is back
        for (auto& item : extraData->items)
        {
            item.translateWeight.ClearValues();
            item.rotateWeight.ClearValues();
            item.scaleWeight.ClearValues();
        }
    }
}

// A blend in progress moves the normalized weights about as far as the ease spinner or sequence weight driving it, and
// is left alone: it keeps its timing. The weights jump when
// - items come and go: ComputeNormalizedWeights only blends the two highest priorities, so with a third one an item
//   attached at no weight can take a bone over at once, and one detached at no weight can hand it over at once; and
//   sequences are switched without a blend (jump loop to landing) or dropped in the middle of one;
// - it divides one vanishing sum by another: the highest priority still easing in while the next has all but eased
//   out swings the weights between the two many times faster than either spinner moves, whatever is below them.
// A jump is taken up by the items' offsets instead: each keeps the weight it was last blended with, and the offsets
// decay from there, so the pose the jump left behind fades out over fBlendSmoothingRate.
void BlendSmoothing::ApplyForItems(kBlendInterpolatorExtraData* extraData,
    const std::vector<std::pair<NiBlendInterpolator::InterpArrayItem*, kBlendInterpItem*>>& items, kWeightType type,
    const Step& step)
{
    if (!g_pluginSettings.blendSmoothing || !extraData || step.bDisabled || items.empty())
        return;

    const auto getWeightState = [type](const std::pair<NiBlendInterpolator::InterpArrayItem*, kBlendInterpItem*>& pair) -> kWeightState*
    {
        auto [itemPtr, extraItemPtr] = pair;
        if (!itemPtr->m_spInterpolator || !extraItemPtr)
            return nullptr;
        DebugAssert(!extraItemPtr->isAdditive);
        return extraItemPtr->GetWeightState(type);
    };

    if (extraData->unsmoothedUpdate)
    {
        for (const auto& pair : items)
        {
            auto* weightState = getWeightState(pair);
            if (!weightState)
                continue;
            weightState->calculatedNormalizedWeight = pair.first->m_fNormalizedWeight;
            weightState->lastSmoothedWeight = pair.first->m_fNormalizedWeight;
            weightState->offset = 0.0f;
        }
        return;
    }

    // weights below this are not blended
    constexpr float MIN_WEIGHT = 0.001f;
    // a change of the weights this small is not worth taking up, however fast
    constexpr float MIN_JUMP = 0.02f;
    // one sequence easing in over another moves the weights up to 1.5 times as far as its ease spinner (sequences
    // easing together count once: step.fInputChange is the item that moved most); from here on it is
    // ComputeNormalizedWeights swinging by itself
    constexpr float MAX_GAIN = 3.0f;

    // an item not blended before had no weight
    const auto getLastWeight = [](const kWeightState& weightState)
    {
        return weightState.calculatedNormalizedWeight == -NI_INFINITY ? 0.0f : weightState.calculatedNormalizedWeight;
    };

    // how much of the weight changed hands since the last evaluation (1: all of it), and how much of that evaluation's
    // weight the items still here held
    float fWeightChange = 0.0f;
    float fLastTotal = 0.0f;
    for (const auto& pair : items)
    {
        if (const auto* weightState = getWeightState(pair))
        {
            const float fLastWeight = getLastWeight(*weightState);
            fWeightChange += std::abs(pair.first->m_fNormalizedWeight - fLastWeight);
            fLastTotal += fLastWeight;
        }
    }
    fWeightChange *= 0.5f;

    // nothing was blended before (first evaluation, after ReloadTargets or noBlendSmoothing): nothing to continue from
    const bool bJump = fLastTotal >= MIN_WEIGHT
        && (step.bItemsChanged || (fWeightChange > MIN_JUMP && fWeightChange > MAX_GAIN * step.fInputChange));
    const float fDecay = std::exp(-step.fDeltaTime / g_pluginSettings.blendSmoothingRate);

    for (const auto& pair : items)
    {
        auto* weightState = getWeightState(pair);
        if (!weightState)
            continue;
        auto& item = *pair.first;
        const float fWeight = item.m_fNormalizedWeight;

        float fOffset = weightState->offset;
        if (bJump)
            fOffset += getLastWeight(*weightState) - fWeight;
        fOffset *= fDecay;
        if (std::abs(fOffset) < MIN_WEIGHT)
            fOffset = 0.0f;

        // an item still easing out may be computed less weight than a jump took from it
        float fSmoothedWeight = max(fWeight + fOffset, 0.0f);
        if (fSmoothedWeight < MIN_WEIGHT)
            fSmoothedWeight = 0.0f;

        weightState->offset = fOffset;
        weightState->calculatedNormalizedWeight = fWeight;
        weightState->lastSmoothedWeight = fSmoothedWeight;
        item.m_fNormalizedWeight = fSmoothedWeight;
    }
}

// Detached items stay in the blend until they have faded out
void BlendSmoothing::DetachZeroWeightItems(kBlendInterpolatorExtraData* extraData, NiBlendInterpolator* blendInterp)
{
    if (!extraData || !g_pluginSettings.blendSmoothing)
        return;
    for (auto& item : blendInterp->GetItems())
    {
        if (!item.m_spInterpolator)
            continue;
        auto* extraItem = extraData->GetItem(item.m_spInterpolator);
        if (!extraItem || extraItem->isAdditive || !extraItem->detached || !extraItem->IsSmoothedWeightZero())
            continue;
        blendInterp->RemoveInterpInfo(item.GetIndex(blendInterp));
        extraItem->ClearValues();
        extraItem->debugState = kInterpDebugState::RemovedInBlendSmoothing;
    }
}

namespace
{
    thread_local std::vector<NiControllerSequence*> g_activeSequences;
    void __fastcall HandleReloadTargets(NiControllerManager* manager)
    {
        // the palette was just refilled from the scene graph; every sequence is bound to it again below
        SkeletonNodes::Restore(manager);

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
