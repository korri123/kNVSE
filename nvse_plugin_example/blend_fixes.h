#pragma once
#include "commands_animation.h"
#include "GameProcess.h"

void FixConflictingPriorities(NiControllerSequence* pkSource, NiControllerSequence* pkDest, NiControllerSequence* pkIdle);

namespace BlendFixes
{
	enum Result
	{
		RESUME, SKIP
	};

	Result ApplyAimBlendFix(AnimData* animData, BSAnimGroupSequence* destAnim);
	void ApplyAttackISToAttackFix();
	void ApplyAttackToAttackISFix();
	void ApplyWeaponStanceFix();
	// The state Animation::Update and StartGroup take a section's current sequence to be in: ANIMATING for a variant
	// TransitionToVariant cross faded in that has taken over from the sequence it continues
	NiControllerSequence::AnimState ContinuedVariantState(const AnimData* animData, const NiControllerSequence* sequence);
	// A sequence easing in that TransitionToVariant cross faded in as a variant of another
	bool ContinuesVariant(const NiControllerSequence& sequence);
	void WriteDelayedHooks();
	void ApplyAimBlendHooks();
	void FixConflictingPriorities(BSAnimGroupSequence* pkSource, BSAnimGroupSequence* pkDest, BSAnimGroupSequence* pkIdle);
	void ApplyHooks();
	void FixPrematureFirstPersonEnd(AnimData* animData, BSAnimGroupSequence* anim);
	void ApplyMissingUpDownAnims(AnimData* animData);
	void AddMissingMTIdleInterps(const AnimData* animData, BSAnimGroupSequence* anim);
}

template <typename F>
BSAnimGroupSequence* GetActiveSequenceWhere(AnimData* animData, F&& predicate)
{
	for (auto* sequence : animData->controllerManager->m_kActiveSequences)
	{
		auto* bsSequence = static_cast<BSAnimGroupSequence*>(sequence);
		if (IS_TYPE(bsSequence, BSAnimGroupSequence) && bsSequence && predicate(bsSequence))
			return bsSequence;
	}
	return nullptr;
}

inline BSAnimGroupSequence* GetActiveSequenceByGroupID(AnimData* animData, AnimGroupID groupId)
{
	return GetActiveSequenceWhere(animData, [&](BSAnimGroupSequence* sequence)
	{
		return sequence->animGroup->GetBaseGroupID() == groupId;
	});
}