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
	void ApplyAimBlendHooks();
	void FixConflictingPriorities(BSAnimGroupSequence* pkSource, BSAnimGroupSequence* pkDest, BSAnimGroupSequence* pkIdle);
	void ApplyHooks();
	void FixPrematureFirstPersonEnd(AnimData* animData, BSAnimGroupSequence* anim);
	void ApplyMissingUpDownAnims(AnimData* animData);
}

