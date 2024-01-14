#pragma once
#include "commands_animation.h"
#include "GameProcess.h"

namespace SpineSnapFix
{
	enum Result
	{
		RESUME, SKIP
	};

	Result ApplyAimBlendFix(AnimData* animData, BSAnimGroupSequence* destAnim);
	Result ApplySamePriorityFix(AnimData* animData, BSAnimGroupSequence* destAnim);
	void ApplyAttackISToAttackFix();
	void ApplyHooks();
}
