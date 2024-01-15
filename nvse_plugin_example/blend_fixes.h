#pragma once
#include "commands_animation.h"
#include "GameProcess.h"

namespace BlendFixes
{
	enum Result
	{
		RESUME, SKIP
	};

	Result ApplyAimBlendFix(AnimData* animData, BSAnimGroupSequence* destAnim);
	void ApplyAttackISToAttackFix();
	void ApplyAimBlendHooks();
}
