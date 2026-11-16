#include "blend_smoothing.h"

#include "additive_anims.h"
#include "blend_curves.h"
#include "GameAPI.h"
#include "hooks.h"
#include "SafeWrite.h"
#include "sequence_extradata.h"
#include "skeleton_nodes.h"
#include "utility.h"

namespace
{
    // Time over which a jump (and, with fBlendSmoothness, a blend in progress) fades out. About a third of it is left
    // after this time.
    constexpr float kFadeTime = 0.075f;
    // Blends shorter than this are cuts requested by the animation (such as the 1 frame attack blend-in) and are not
    // smoothed by fBlendSmoothness, since smoothing them would delay the start of a shot and weaken its recoil.
    constexpr float kMinSmoothedBlendTime = 0.08f;
}

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
    this->parentSwitch.~unique_ptr();
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
    new(&extraData->parentSwitch) std::unique_ptr<WeaponParentSwitch::NodeState>();
    // NiNew does not run constructors
    extraData->accumRootChild = false;
    extraData->hasComputedRotate = false;
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
    // Most bones have up to 4 items (aim, its up/down variants, a gait or attack): one allocation instead of four
    if (items.empty() && items.capacity() == 0)
        items.reserve(4);
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
    // Not evaluated before, or the animation time went back: assume one frame
    if (fLastTime == -NI_INFINITY || fTime < fLastTime)
        return g_timeGlobal->secondsPassed;
    return fTime - fLastTime;
}

void BlendSmoothing::Step::AddItem(const NiBlendInterpolator::InterpArrayItem& item, kBlendInterpItem* extraItem,
    const NiQuatTransform& kTransform)
{
    // A detached item is only fading out. No sequence has it attached, so nothing moves its weight or ease spinner.
    if (extraItem && extraItem->detached)
        return;

    // The valid channels are part of the signature, since each channel is blended from the items that have it. The
    // terms are summed so that the order of the items does not matter.
    const UInt32 uiChannels = (kTransform.IsTranslateValid() ? 1 : 0) | (kTransform.IsRotateValid() ? 2 : 0)
        | (kTransform.IsScaleValid() ? 4 : 0);
    m_uiAttachedItems += (reinterpret_cast<UInt32>(item.m_spInterpolator.data) * 2654435761u)
        ^ (((static_cast<UInt8>(item.m_cPriority) << 3) | uiChannels) * 2246822519u);

    if (!extraItem)
        return;
    if (g_pluginSettings.blendSmoothness > 0.0f && !m_bNamedCurve && extraItem->sequence)
        m_bNamedCurve = BlendCurves::HasNamedCurve(*extraItem->sequence);
    if (extraItem->lastWeight != -NI_INFINITY)
    {
        const float fChange = std::abs(item.m_fWeight - extraItem->lastWeight)
            + std::abs(item.m_fEaseSpinner - extraItem->lastEaseSpinner);
        fInputChange = max(fInputChange, fChange);
    }
    extraItem->lastWeight = item.m_fWeight;
    extraItem->lastEaseSpinner = item.m_fEaseSpinner;
}

// The children of the accum root (Bip01 NonAccum) are posed relative to the actor. The engine turns the actor at once
// when it sits down or stands up, and the furniture animations are authored for the facing after the turn. The turn
// comes with the switch of sequences, without a blend: the actor's angle is set before the evaluation that blends the
// switch, and its 3D is turned after it. Smoothing that switch would pose the node relative to the old facing, so the
// body would flip around and turn back over kFadeTime. Returns true in that case.
static bool ActorTurnsAtOnce(const kBlendInterpolatorExtraData* extraData)
{
    NiAVObject* target = extraData->target;
    if (!extraData->accumRootChild || !target || !target->m_pkParent)
        return false;
    auto* actor = TESObjectREFR::FindReferenceFor3D(target->m_pkParent);
    if (!actor || !actor->IsActor())
        return false;
    // Compares the facing of the 3D the node is posed in with the actor's angle. Walking or turning in place stays well
    // below this per frame, while the engine turns actors onto furniture by multiples of 90 degrees.
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

    // noSmoothing applies from attach to detach, whether or not the item is blended this time
    bDisabled = ra::any_of(extraData->items, [](const kBlendInterpItem& item) { return item.noSmoothing && !item.detached; })
        || ActorTurnsAtOnce(extraData);
    if (bDisabled)
    {
        // Items still fading out are cut (DetachZeroWeightItems) and smoothing starts over once it is enabled again
        for (auto& item : extraData->items)
        {
            item.translateWeight.ClearValues();
            item.rotateWeight.ClearValues();
            item.scaleWeight.ClearValues();
        }
    }
    // An ease spinner moves by 1 over the blend's time (a transition moves the weight instead)
    bSmoothBlend = g_pluginSettings.blendSmoothness > 0.0f && !m_bNamedCurve
        && fInputChange * kMinSmoothedBlendTime <= fDeltaTime;
}

// Smooths jumps in the normalized weights of a blend interpolator. A blend in progress moves the weights about as far
// as the ease spinner or sequence weight driving it. It is left alone, and keeps its timing, except for the share
// fBlendSmoothness takes up (see below). The weights jump when
// - items come and go: ComputeNormalizedWeights only blends the two highest priorities, so with a third one an item
//   attached at no weight can take a bone over at once, and one detached at no weight can hand it over at once;
//   sequences can also be switched without a blend (jump loop to landing) or dropped in the middle of one;
// - one vanishing sum is divided by another: while the highest priority eases in and the next has all but eased out,
//   the weights swing between the two far faster than either spinner moves.
// Each item's offset absorbs a jump: the item keeps the weight it was last blended with and the offset decays from
// there, so the pose left behind fades out over kFadeTime.
// fBlendSmoothness (0..1) absorbs that share of every other change of the weights in the same way. At 1 the bone
// follows the computed weights through a low pass of kFadeTime, so blends slow down into the new pose and end later
// than the animations specify. Values in between mix that with the computed weights. The change is linear in the
// weights, so they still sum to what was computed. Only the blended weights change, not the sequences' states or times.
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

    // Weights below this are not blended
    constexpr float MIN_WEIGHT = 0.001f;
    // Weight changes below this are not worth absorbing, however fast
    constexpr float MIN_JUMP = 0.02f;
    // A sequence easing in over another moves the weights up to 1.5 times as far as its ease spinner (sequences easing
    // together count once: step.fInputChange is the item that moved most). Above this gain, the change comes from
    // ComputeNormalizedWeights swinging by itself.
    constexpr float MAX_GAIN = 3.0f;

    // An item that was not blended before had no weight
    const auto getLastWeight = [](const kWeightState& weightState)
    {
        return weightState.calculatedNormalizedWeight == -NI_INFINITY ? 0.0f : weightState.calculatedNormalizedWeight;
    };

    // fWeightChange: fraction of the weight that changed hands since the last evaluation (1: all of it).
    // fLastTotal: how much of that evaluation's weight the remaining items held.
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

    // Nothing was blended before (first evaluation, after ReloadTargets or noBlendSmoothing), so there is nothing to continue
    const bool bJump = fLastTotal >= MIN_WEIGHT
        && (step.bItemsChanged || (fWeightChange > MIN_JUMP && fWeightChange > MAX_GAIN * step.fInputChange));
    // Share of this evaluation's change that the offsets absorb
    float fTakeUp = 0.0f;
    if (bJump)
        fTakeUp = 1.0f;
    else if (step.bSmoothBlend && fLastTotal >= MIN_WEIGHT)
        fTakeUp = g_pluginSettings.blendSmoothness;
    const float fDecay = std::exp(-step.fDeltaTime / kFadeTime);

    for (const auto& pair : items)
    {
        auto* weightState = getWeightState(pair);
        if (!weightState)
            continue;
        auto& item = *pair.first;
        const float fWeight = item.m_fNormalizedWeight;

        float fOffset = weightState->offset;
        fOffset += fTakeUp * (getLastWeight(*weightState) - fWeight);
        fOffset *= fDecay;
        if (std::abs(fOffset) < MIN_WEIGHT)
            fOffset = 0.0f;

        // An item still easing out can be computed less weight than a jump took from it
        float fSmoothedWeight = max(fWeight + fOffset, 0.0f);
        if (fSmoothedWeight < MIN_WEIGHT)
            fSmoothedWeight = 0.0f;

        weightState->offset = fOffset;
        weightState->calculatedNormalizedWeight = fWeight;
        weightState->lastSmoothedWeight = fSmoothedWeight;
        item.m_fNormalizedWeight = fSmoothedWeight;
    }
}

// Detached items stay in the blend until they have faded out, then this removes them
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
        // The palette was just refilled from the scene graph. Every sequence is bound to it again below.
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
