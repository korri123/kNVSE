#include "movement_blend_fixes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "anim_switch.h"
#include "blend_fixes.h"
#include "hooks.h"

void SetAnimDataState(AnimData* animData, BSAnimGroupSequence* pkSequence)
{
	constexpr auto sequenceId = kSequence_Movement;
	animData->groupIDs[sequenceId] = pkSequence->animGroup->groupID;
	animData->animSequence[sequenceId] = pkSequence;
	animData->sequenceState1[sequenceId] = 0;
}

void FixJumpLoopMovementSpeed(AnimData* animData, BSAnimGroupSequence* pkSequence)
{
	if (pkSequence->animGroup->GetBaseGroupID() == kAnimGroup_JumpLoop && animData != g_thePlayer->firstPersonAnimData)
	{
		auto* forwardAnim = GetAnimByGroupID(animData, kAnimGroup_Forward);
		if (forwardAnim && forwardAnim->animGroup)
			animData->movementSpeedMult = BSGlobals::walkSpeed / forwardAnim->animGroup->moveVector.Length() * animData->actor->GetWalkSpeedMult();
	}
}

namespace
{
	// Walk and run cycles (3rd person): the movement groups whose variants (weapon type, sneak, power armor: the group ID's
	// high byte) are the same alternating steps
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
	// both feet relative to their own mean over the cycle, in the thighs' parent space
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

	// A node's local transform as the interpolator animates it at fTime; channels it does not animate keep the node's
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

	// The feet over one cycle of the sequence. Interpolators are shared by every clone of a KF and cache their last
	// evaluation by time: sampling other times must not leave that cache claiming them (as WeaponHandBlend does).
	bool SampleFeet(const NiControllerSequence* sequence, const Legs& legs, FeetCycle& cycle)
	{
		std::array<std::array<NiInterpolator*, kLegBones>, 2> interpolators{};
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
		const float fBegin = sequence->m_fBeginKeyTime;
		const float fLength = sequence->m_fEndKeyTime - fBegin;
		std::array<NiPoint3, 2> mean{};
		for (unsigned int sample = 0; sample < kGaitSamples; ++sample)
		{
			const float fTime = fBegin + fLength * static_cast<float>(sample) / kGaitSamples;
			for (unsigned int leg = 0; leg < 2; ++leg)
			{
				NiTransform chain = SampleLocal(interpolators[leg][0], legs[leg][0], fTime);
				for (unsigned int bone = 1; bone < kLegBones; ++bone)
					chain = chain * SampleLocal(interpolators[leg][bone], legs[leg][bone], fTime);
				cycle[sample][leg] = chain.m_Translate;
				mean[leg] += chain.m_Translate;
			}
		}
		for (const auto& legInterpolators : interpolators)
			for (auto* interpolator : legInterpolators)
				interpolator->m_fLastTime = -NI_INFINITY;
		for (auto& feet : cycle)
			for (unsigned int leg = 0; leg < 2; ++leg)
				feet[leg] -= mean[leg] / static_cast<float>(kGaitSamples);
		return true;
	}

	// The fraction of a cycle by which the destination's steps lag the source's: where the feet of both cycles match
	// best. Variants of a gait don't share where their cycle starts (h2hforward, sneakmtforward and 2hhbackward are half
	// a cycle from mtforward/mtbackward) nor how their footstep text keys are placed.
	float FindCycleOffset(const FeetCycle& source, const FeetCycle& dest)
	{
		std::array<float, kGaitSamples> costs{};
		unsigned int best = 0;
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
			if (cost < costs[best])
				best = shift;
		}
		// between samples: the vertex of the parabola through the best shift and its neighbours
		const float fBefore = costs[(best + kGaitSamples - 1) % kGaitSamples];
		const float fAfter = costs[(best + 1) % kGaitSamples];
		const float fCurvature = fBefore - 2.0f * costs[best] + fAfter;
		const float fDelta = fCurvature > 0.0f ? std::clamp(0.5f * (fBefore - fAfter) / fCurvature, -0.5f, 0.5f) : 0.0f;
		return (static_cast<float>(best) + fDelta) / kGaitSamples;
	}

	// The time in pkDest's cycle at which its steps match where pkSource is in its cycle, or NAN when either is not a
	// variant of the same walk/run cycle with leg tracks
	float FindMatchingGaitTime(AnimData* animData, const BSAnimGroupSequence* pkSource, const BSAnimGroupSequence* pkDest)
	{
		if (!IsGaitCycle(pkSource) || !IsGaitCycle(pkDest) || pkSource->animGroup->GetBaseGroupID() != pkDest->animGroup->GetBaseGroupID())
			return NAN;
		if (pkSource->m_fLastTime == -NI_INFINITY)
			return NAN; // not evaluated since it was started: no time of its own yet
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

	// m: text keys (the engine's morph tag, NiControllerSequence::pMorphTag): whole keys starting with "m:" in any case,
	// points authored to match across movement cycles (m:L / m:R where a foot is planted). Key texts are never null here:
	// AnimFixes::EraseNullTextKeys drops those when the KF loads (KFModel::InitSequence), before it gets an anim group
	bool IsMorphKey(const NiTextKey& key)
	{
		return _strnicmp(key.m_kText, "m:", 2) == 0;
	}

	// A looping cycle with m: keys: FindCorrespondingMorphFrame maps between such cycles
	bool IsMorphCycle(const NiControllerSequence* sequence)
	{
		return sequence->m_spTextKeys && sequence->m_eCycleType == NiControllerSequence::LOOP &&
			sequence->m_fEndKeyTime > sequence->m_fBeginKeyTime &&
			std::ranges::any_of(sequence->m_spTextKeys->GetKeys(), IsMorphKey);
	}

	// Every m: key of from has a key of the same text (case-sensitive, as the engine compares them) in to
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

	// Whether pkDest can start at pkSource's point between their m: keys and pkSource then follow it: both have the same
	// set of m: keys (NiControllerSequence::VerifyMatchingMorphKeys) and pkSource runs on a clock of its own. The game
	// (Animation::StartGroup) also requires the same number of controlled bones and the same character after the first
	// m: key's "m:" (TESAnimGroup's morph type, set in BSAnimGroupSequence::InitAnimGroup), which lets few authored pairs
	// through: vanilla mtfastforward starts with m:R, mtbackward with m:L
	bool CanFollowMorphKeys(const NiControllerSequence* pkSource, const NiControllerSequence* pkDest)
	{
		if (pkSource->m_fLastTime == -NI_INFINITY || pkSource->m_fDestFrame != -NI_INFINITY || pkSource->m_pkPartnerSequence)
			return false;
		return IsMorphCycle(pkSource) && IsMorphCycle(pkDest) &&
			HasMorphKeysOf(pkDest, pkSource) && HasMorphKeysOf(pkSource, pkDest);
	}
}

BSAnimGroupSequence* MovementBlendFixes::PlayMovementAnim(AnimData* animData, BSAnimGroupSequence* pkSequence)
{
	FixJumpLoopMovementSpeed(animData, pkSequence);
	
	const float fDuration = pkSequence->GetEaseInTime();
	auto* currentMoveAnim = animData->animSequence[kSequence_Movement];
	if (!currentMoveAnim)
	{
		currentMoveAnim = GetActiveSequenceWhere(animData, [](const BSAnimGroupSequence* seq)
		{
			return seq->animGroup->GetSequenceType() == kSequence_Movement && seq->m_eState != NiControllerSequence::EASEOUT;
		});
	}

	const float fTime = animData->timePassed;
	// started again or eased back in: sequences still following its cycle from an earlier switch would jump with it
	if (pkSequence->m_eState == NiControllerSequence::INACTIVE || pkSequence->m_eState == NiControllerSequence::EASEOUT)
		pkSequence->ReleaseFollowers(fTime);

	// a sequence started from inactive cross fades from the point the playing one is at: between their m: keys where
	// both have the same, else for a variant of the gait playing (weapon type switched at an equip's Attach or unequip's
	// Detach key, sneaking, ...) at the same step
	float fStartTime = NAN;
	bool bFollowMorphKeys = false;
	if (currentMoveAnim && currentMoveAnim != pkSequence && pkSequence->m_eState == NiControllerSequence::INACTIVE &&
		(currentMoveAnim->m_eState == NiControllerSequence::ANIMATING || currentMoveAnim->m_eState == NiControllerSequence::EASEIN))
	{
		if (g_pluginSettings.fixMovementMorphKeys && CanFollowMorphKeys(currentMoveAnim, pkSequence))
		{
			// as the engine's morph does (NiControllerSequence::Update, MORPHSOURCE)
			fStartTime = pkSequence->FindCorrespondingMorphFrame(currentMoveAnim, currentMoveAnim->m_fOffset + fTime);
			bFollowMorphKeys = true;
			// it will follow pkSequence: the sequences following it would read a clock it no longer shows
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
	// only set for a sequence started from inactive
	if (!std::isnan(fStartTime))
		pkSequence->m_fOffset = fStartTime / pkSequence->m_fFrequency - fTime;
	// the old sequence takes its time from the new one's at their m: keys while it fades out (NiControllerSequence::Update),
	// as the source of the engine's morph: both stay in step however long their cycles are. Deactivate drops the partner
	if (bFollowMorphKeys && currentMoveAnim->m_eState == NiControllerSequence::EASEOUT)
		currentMoveAnim->m_pkPartnerSequence = pkSequence;

	SetAnimDataState(animData, pkSequence);
	return pkSequence;
}

// when a sequence's ease-out ends, in its owner's animation time
static float GetEaseOutEndTime(const NiControllerSequence* sequence, float fTime)
{
	// Deactivate leaves the duration in m_fEndTime until the next update turns it into a time
	return sequence->m_fStartTime == -NI_INFINITY ? fTime + sequence->m_fEndTime : sequence->m_fEndTime;
}

// Animation::ClearGroup (0x496080) remembers the movement sequence it deactivates (animSeq100), and while that one eases
// out with no movement group playing, Animation::Update (0x491180) drops the movement delta. PlayMovementAnim's cross
// fade leaves the previous movement sequence easing out as well: when the remembered one ended first (a stop within
// half the blend, as on every analog stick release: run -> walk -> stop), the other's root motion moved the actor
// until its ease-out ended, a lunge at the animation's unscaled speed.
// ClearGroup copies animSequence[kSequence_Movement] right after deactivating it: leave the one that ends last there.
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

