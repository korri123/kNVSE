#include "movement_blend_fixes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "anim_switch.h"
#include "blend_fixes.h"
#include "hooks.h"
#include "sequence_extradata.h"

void SetAnimDataState(AnimData* animData, BSAnimGroupSequence* pkSequence)
{
	constexpr auto sequenceId = kSequence_Movement;
	animData->groupIDs[sequenceId] = pkSequence->animGroup->groupID;
	animData->animSequence[sequenceId] = pkSequence;
	animData->sequenceState1[sequenceId] = 0;
}

void FixJumpLoopMovementSpeed(AnimData* animData, BSAnimGroupSequence* pkSequence)
{
	if (pkSequence->animGroup->GetBaseGroupID() == kAnimGroup_JumpLoop && animData != PlayerCharacter::GetSingleton()->firstPersonAnimData)
	{
		auto* forwardAnim = GetAnimByGroupID(animData, kAnimGroup_Forward);
		if (forwardAnim && forwardAnim->animGroup)
			animData->movementSpeedMult = BSGlobals::walkSpeed / forwardAnim->animGroup->moveVector.Length() * animData->actor->GetWalkSpeedMult();
	}
}

namespace
{
	// Returns true for walk and run cycles (3rd person). Weapon type, sneak and power armor variants (the group ID's
	// high byte) of a movement group alternate steps in the same way.
	bool IsGaitCycle(const BSAnimGroupSequence* sequence)
	{
		if (!sequence->animGroup || sequence->m_eCycleType != NiControllerSequence::LOOP ||
			sequence->m_fEndKeyTime <= sequence->m_fBeginKeyTime)
			return false;
		const auto groupId = sequence->animGroup->GetBaseGroupID();
		return groupId >= kAnimGroup_Forward && groupId <= kAnimGroup_FastRight;
	}

	constexpr unsigned int kGaitSamples = 32;
	constexpr unsigned int kLegBones = 3;
	using Legs = std::array<std::array<NiAVObject*, kLegBones>, 2>;
	using LegInterpolators = std::array<std::array<NiInterpolator*, kLegBones>, 2>;
	// Both feet relative to their own mean over the cycle, in the thighs' parent space
	using FeetCycle = std::array<std::array<NiPoint3, 2>, kGaitSamples>;

	bool GetLegs(AnimData* animData, Legs& legs)
	{
		// BSUtilities::GetObjectByName interns its char* in the global string table on every call
		static const NiFixedString names[2][kLegBones] = {
			{ "Bip01 L Thigh", "Bip01 L Calf", "Bip01 L Foot" },
			{ "Bip01 R Thigh", "Bip01 R Calf", "Bip01 R Foot" },
		};
		if (!animData->nBip01)
			return false;
		for (unsigned int leg = 0; leg < 2; ++leg)
			for (unsigned int bone = 0; bone < kLegBones; ++bone)
				if (!(legs[leg][bone] = animData->nBip01->GetObjectByName(names[leg][bone])))
					return false;
		return true;
	}

	bool GetLegInterpolators(const NiControllerSequence* sequence, const Legs& legs, LegInterpolators& interpolators)
	{
		for (unsigned int leg = 0; leg < 2; ++leg)
		{
			for (unsigned int bone = 0; bone < kLegBones; ++bone)
			{
				const auto* block = sequence->GetControlledBlock(legs[leg][bone]->m_pcName);
				if (!block || !block->m_spInterpolator)
					return false;
				interpolators[leg][bone] = block->m_spInterpolator;
			}
		}
		return true;
	}

	// Interpolators are shared by every clone of a KF and cache their last evaluation time. Sampling other times must
	// not leave a stale cache behind (WeaponHandBlend does the same).
	void ResetLastTimes(const LegInterpolators& interpolators)
	{
		for (const auto& legInterpolators : interpolators)
			for (auto* interpolator : legInterpolators)
				interpolator->m_fLastTime = -NI_INFINITY;
	}

	// Returns a node's local transform as the interpolator animates it at fTime. Channels the interpolator doesn't
	// animate keep the node's current value.
	NiTransform SampleLocal(NiInterpolator* interpolator, NiAVObject* node, float fTime)
	{
		NiTransform result = node->m_kLocal;
		NiQuatTransform value;
		if (interpolator->Update(fTime, node, value))
		{
			if (value.IsTranslateValid())
				result.m_Translate = value.GetTranslate();
			if (value.IsRotateValid())
			{
				NiQuaternion rotation = value.GetRotate();
				rotation.Normalize();
				rotation.ToRotation(result.m_Rotate);
			}
		}
		return result;
	}

	// Both feet in the thighs' parent space, from the leg bones' local transforms
	template <typename GetLocal>
	std::array<NiPoint3, 2> GetFeet(const Legs& legs, GetLocal&& getLocal)
	{
		std::array<NiPoint3, 2> feet;
		for (unsigned int leg = 0; leg < 2; ++leg)
		{
			NiTransform chain = getLocal(leg, 0);
			for (unsigned int bone = 1; bone < kLegBones; ++bone)
				chain = chain * getLocal(leg, bone);
			feet[leg] = chain.m_Translate;
		}
		return feet;
	}

	std::array<NiPoint3, 2> SampleFeetAt(const LegInterpolators& interpolators, const Legs& legs, float fTime)
	{
		return GetFeet(legs, [&](unsigned int leg, unsigned int bone)
		{
			return SampleLocal(interpolators[leg][bone], legs[leg][bone], fTime);
		});
	}

	// The left foot relative to the right one: where the feet are in the step, independent of the pelvis
	NiPoint3 GetStride(const std::array<NiPoint3, 2>& feet)
	{
		return feet[0] - feet[1];
	}

	// The stride of the skeleton's last evaluated pose
	NiPoint3 GetCurrentStride(const Legs& legs)
	{
		return GetStride(GetFeet(legs, [&](unsigned int leg, unsigned int bone) { return legs[leg][bone]->m_kLocal; }));
	}

	// The feet over one cycle of the sequence
	bool SampleFeet(const NiControllerSequence* sequence, const Legs& legs, FeetCycle& cycle)
	{
		LegInterpolators interpolators;
		if (!GetLegInterpolators(sequence, legs, interpolators))
			return false;
		const float fBegin = sequence->m_fBeginKeyTime;
		const float fLength = sequence->m_fEndKeyTime - fBegin;
		std::array<NiPoint3, 2> mean{};
		for (unsigned int sample = 0; sample < kGaitSamples; ++sample)
		{
			cycle[sample] = SampleFeetAt(interpolators, legs, fBegin + fLength * static_cast<float>(sample) / kGaitSamples);
			for (unsigned int leg = 0; leg < 2; ++leg)
				mean[leg] += cycle[sample][leg];
		}
		ResetLastTimes(interpolators);
		for (auto& feet : cycle)
			for (unsigned int leg = 0; leg < 2; ++leg)
				feet[leg] -= mean[leg] / static_cast<float>(kGaitSamples);
		return true;
	}

	// Returns the fraction of a cycle at which the evenly sampled costs are lowest, interpolated between samples with
	// the vertex of the parabola through the lowest sample and its neighbours.
	float FindCyclicMinimum(const std::array<float, kGaitSamples>& costs)
	{
		const auto best = static_cast<unsigned int>(std::ranges::min_element(costs) - costs.begin());
		const float fBefore = costs[(best + kGaitSamples - 1) % kGaitSamples];
		const float fAfter = costs[(best + 1) % kGaitSamples];
		const float fCurvature = fBefore - 2.0f * costs[best] + fAfter;
		const float fDelta = fCurvature > 0.0f ? std::clamp(0.5f * (fBefore - fAfter) / fCurvature, -0.5f, 0.5f) : 0.0f;
		const float fFraction = (static_cast<float>(best) + fDelta) / kGaitSamples;
		return fFraction - std::floor(fFraction);
	}

	// Returns the fraction of a cycle by which the destination's steps lag the source's, i.e. where the feet of both
	// cycles match best. Gait variants don't share where their cycle starts (h2hforward, sneakmtforward and
	// 2hhbackward are half a cycle from mtforward/mtbackward) or how their footstep text keys are placed.
	float FindCycleOffset(const FeetCycle& source, const FeetCycle& dest)
	{
		std::array<float, kGaitSamples> costs{};
		for (unsigned int shift = 0; shift < kGaitSamples; ++shift)
		{
			float cost = 0.0f;
			for (unsigned int sample = 0; sample < kGaitSamples; ++sample)
			{
				const auto& dst = dest[(sample + shift) % kGaitSamples];
				for (unsigned int leg = 0; leg < 2; ++leg)
					cost += (source[sample][leg] - dst[leg]).SqrLength();
			}
			costs[shift] = cost;
		}
		return FindCyclicMinimum(costs);
	}

	// Returns the time in pkDest's cycle at which its steps match where pkSource is in its cycle. Returns NAN if either
	// is not a variant of the same walk/run cycle, or lacks leg tracks.
	float FindMatchingGaitTime(AnimData* animData, const BSAnimGroupSequence* pkSource, const BSAnimGroupSequence* pkDest)
	{
		if (!IsGaitCycle(pkSource) || !IsGaitCycle(pkDest) || pkSource->animGroup->GetBaseGroupID() != pkDest->animGroup->GetBaseGroupID())
			return NAN;
		if (pkSource->m_fLastTime == -NI_INFINITY)
			return NAN; // Not evaluated since it started, so it has no time of its own yet
		Legs legs;
		FeetCycle source, dest;
		if (!GetLegs(animData, legs) || !SampleFeet(pkSource, legs, source) || !SampleFeet(pkDest, legs, dest))
			return NAN;
		const float fSourceLength = pkSource->m_fEndKeyTime - pkSource->m_fBeginKeyTime;
		const float fSourcePhase = (pkSource->m_fLastScaledTime - pkSource->m_fBeginKeyTime) / fSourceLength;
		float fPhase = fSourcePhase + FindCycleOffset(source, dest);
		fPhase -= std::floor(fPhase);
		return pkDest->m_fBeginKeyTime + fPhase * (pkDest->m_fEndKeyTime - pkDest->m_fBeginKeyTime);
	}

	// Returns the time in the gait's cycle whose stride best matches the skeleton's pose, or NAN without leg tracks.
	// Used after a start or stop animation, which is a pose to continue from, not a cycle to line up with.
	float FindGaitTimeMatchingPose(AnimData* animData, const BSAnimGroupSequence* gait)
	{
		Legs legs;
		LegInterpolators interpolators;
		if (!IsGaitCycle(gait) || !GetLegs(animData, legs) || !GetLegInterpolators(gait, legs, interpolators))
			return NAN;
		const NiPoint3 stride = GetCurrentStride(legs);
		const float fBegin = gait->m_fBeginKeyTime;
		const float fLength = gait->m_fEndKeyTime - fBegin;
		std::array<float, kGaitSamples> costs{};
		for (unsigned int sample = 0; sample < kGaitSamples; ++sample)
		{
			const float fTime = fBegin + fLength * static_cast<float>(sample) / kGaitSamples;
			costs[sample] = (GetStride(SampleFeetAt(interpolators, legs, fTime)) - stride).SqrLength();
		}
		ResetLastTimes(interpolators);
		return fBegin + fLength * FindCyclicMinimum(costs);
	}

	bool IsMovementTransition(const NiControllerSequence* sequence)
	{
		const auto* extraData = SequenceExtraDatas::Get(sequence);
		return extraData && !extraData->bAimTransition && extraData->transition != TransitionKind::None;
	}

	// "m:" text keys (the engine's morph tag, NiControllerSequence::pMorphTag): keys starting with "m:" in any case.
	// They are authored to match points across movement cycles (m:L / m:R where a foot is planted). Key texts are
	// never null here because AnimFixes::EraseNullTextKeys drops those when the KF loads (KFModel::InitSequence).
	bool IsMorphKey(const NiTextKey& key)
	{
		return _strnicmp(key.m_kText, "m:", 2) == 0;
	}

	// Returns true for a looping cycle with m: keys, which FindCorrespondingMorphFrame can map between
	bool IsMorphCycle(const NiControllerSequence* sequence)
	{
		return sequence->m_spTextKeys && sequence->m_eCycleType == NiControllerSequence::LOOP &&
			sequence->m_fEndKeyTime > sequence->m_fBeginKeyTime &&
			std::ranges::any_of(sequence->m_spTextKeys->GetKeys(), IsMorphKey);
	}

	// Returns true if every m: key of `from` has a key with the same text in `to` (case-sensitive, as in the engine)
	bool HasMorphKeysOf(const NiControllerSequence* to, const NiControllerSequence* from)
	{
		const auto toKeys = to->m_spTextKeys->GetKeys();
		return std::ranges::all_of(from->m_spTextKeys->GetKeys(), [&](const NiTextKey& key)
		{
			return !IsMorphKey(key) || std::ranges::any_of(toKeys, [&](const NiTextKey& other)
			{
				return strcmp(other.m_kText, key.m_kText) == 0;
			});
		});
	}

	// Returns true if pkDest can start at pkSource's point between their m: keys and pkSource can then follow it. Both
	// need the same set of m: keys (NiControllerSequence::VerifyMatchingMorphKeys) and pkSource must run on its own
	// clock. The game (Animation::StartGroup) additionally requires the same number of controlled bones and the same
	// character after the first m: key's "m:" (TESAnimGroup's morph type, set in BSAnimGroupSequence::InitAnimGroup).
	// Few authored pairs pass that: vanilla mtfastforward starts with m:R and mtbackward with m:L.
	bool CanFollowMorphKeys(const NiControllerSequence* pkSource, const NiControllerSequence* pkDest)
	{
		if (pkSource->m_fLastTime == -NI_INFINITY || pkSource->m_fDestFrame != -NI_INFINITY || pkSource->m_pkPartnerSequence)
			return false;
		return IsMorphCycle(pkSource) && IsMorphCycle(pkDest) &&
			HasMorphKeysOf(pkDest, pkSource) && HasMorphKeysOf(pkSource, pkDest);
	}
}

BSAnimGroupSequence* MovementBlendFixes::PlayMovementAnim(AnimData* animData, BSAnimGroupSequence* pkSequence, float fDuration)
{
	FixJumpLoopMovementSpeed(animData, pkSequence);
	
	if (std::isnan(fDuration))
		fDuration = pkSequence->GetEaseInTime();
	auto* currentMoveAnim = animData->animSequence[kSequence_Movement];
	if (!currentMoveAnim)
	{
		currentMoveAnim = GetActiveSequenceWhere(animData, [](const BSAnimGroupSequence* seq)
		{
			return seq->animGroup->GetSequenceType() == kSequence_Movement && seq->m_eState != NiControllerSequence::EASEOUT;
		});
	}

	const float fTime = animData->timePassed;
	// A sequence that is started again or eased back in would take sequences still following its cycle with it
	if (pkSequence->m_eState == NiControllerSequence::INACTIVE || pkSequence->m_eState == NiControllerSequence::EASEOUT)
		pkSequence->ReleaseFollowers(fTime);

	// A sequence started from inactive cross fades from the point the playing one is at:
	// - between their m: keys if both have the same ones
	// - at the same step for a variant of the playing gait (weapon type switched at an equip's Attach or unequip's
	//   Detach key, sneaking, ...)
	// - at the step the legs are at after a start or stop animation
	// Start and stop animations themselves play from their beginning.
	float fStartTime = NAN;
	bool bFollowMorphKeys = false;
	if (currentMoveAnim && currentMoveAnim != pkSequence && pkSequence->m_eState == NiControllerSequence::INACTIVE &&
		(currentMoveAnim->m_eState == NiControllerSequence::ANIMATING || currentMoveAnim->m_eState == NiControllerSequence::EASEIN) &&
		!IsMovementTransition(pkSequence))
	{
		if (IsMovementTransition(currentMoveAnim))
		{
			fStartTime = FindGaitTimeMatchingPose(animData, pkSequence);
		}
		else if (g_pluginSettings.fixMovementMorphKeys && CanFollowMorphKeys(currentMoveAnim, pkSequence))
		{
			// As the engine's morph does (NiControllerSequence::Update, MORPHSOURCE)
			fStartTime = pkSequence->FindCorrespondingMorphFrame(currentMoveAnim, currentMoveAnim->m_fOffset + fTime);
			bFollowMorphKeys = true;
			// The sequences following currentMoveAnim would read a clock it no longer shows once it follows pkSequence
			currentMoveAnim->ReleaseFollowers(fTime);
		}
		else if (g_pluginSettings.fixGaitSwitch)
		{
			fStartTime = FindMatchingGaitTime(animData, currentMoveAnim, pkSequence);
		}
	}

	const auto animSwitch = AnimSwitch::Current(animData);
	if (currentMoveAnim && currentMoveAnim != pkSequence)
		animSwitch.EaseOut(currentMoveAnim, fDuration);
	if (!animSwitch.EaseIn(pkSequence, fDuration))
		return pkSequence;
	// Only set for a sequence started from inactive
	if (!std::isnan(fStartTime))
		pkSequence->m_fOffset = fStartTime / pkSequence->m_fFrequency - fTime;
	// While it fades out, the old sequence takes its time from the new one at their m: keys
	// (NiControllerSequence::Update), like the source of the engine's morph, so both stay in step whatever their cycle
	// lengths. Deactivate drops the partner.
	if (bFollowMorphKeys && currentMoveAnim->m_eState == NiControllerSequence::EASEOUT)
		currentMoveAnim->m_pkPartnerSequence = pkSequence;

	SetAnimDataState(animData, pkSequence);
	return pkSequence;
}

// Returns the time at which a sequence's ease-out ends, in its owner's animation time
static float GetEaseOutEndTime(const NiControllerSequence* sequence, float fTime)
{
	// Deactivate leaves the duration in m_fEndTime until the next update converts it to a time
	return sequence->m_fStartTime == -NI_INFINITY ? fTime + sequence->m_fEndTime : sequence->m_fEndTime;
}

// Animation::ClearGroup (0x496080) remembers the movement sequence it deactivates (animSeq100). While that one eases
// out with no movement group playing, Animation::Update (0x491180) drops the movement delta. PlayMovementAnim's cross
// fade leaves the previous movement sequence easing out as well. If the remembered one ends first (a stop within half
// the blend, e.g. an analog stick release: run -> walk -> stop), the other's root motion moves the actor at the
// animation's unscaled speed until its ease-out ends.
// ClearGroup copies animSequence[kSequence_Movement] right after deactivating it, so this leaves the sequence that
// ends last there.
void MovementBlendFixes::TrackLastEaseOut(AnimData* animData)
{
	BSAnimGroupSequence* last = nullptr;
	float fLastEndTime = -NI_INFINITY;
	for (auto* sequence : animData->controllerManager->m_kActiveSequences)
	{
		auto* bsSequence = static_cast<BSAnimGroupSequence*>(sequence);
		if (!bsSequence || NOT_TYPE(bsSequence, BSAnimGroupSequence) || bsSequence->m_eState != NiControllerSequence::EASEOUT)
			continue;
		if (!bsSequence->animGroup || bsSequence->animGroup->GetSequenceType() != kSequence_Movement)
			continue;
		const float fEndTime = GetEaseOutEndTime(bsSequence, animData->timePassed);
		if (fEndTime > fLastEndTime)
		{
			last = bsSequence;
			fLastEndTime = fEndTime;
		}
	}
	if (last)
		animData->animSequence[kSequence_Movement] = last;
}


float MovementBlendFixes::GetStrideMismatch(AnimData* animData, const NiControllerSequence* sequence, float fTime)
{
	Legs legs;
	LegInterpolators interpolators;
	if (!GetLegs(animData, legs) || !GetLegInterpolators(sequence, legs, interpolators))
		return NAN;
	const NiPoint3 stride = GetStride(SampleFeetAt(interpolators, legs, fTime));
	ResetLastTimes(interpolators);
	return (stride - GetCurrentStride(legs)).SqrLength();
}
