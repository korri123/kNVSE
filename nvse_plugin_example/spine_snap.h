#pragma once
#include "commands_animation.h"
#include "GameProcess.h"

namespace SpineSnapFix
{
	enum Result
	{
		RESUME, SKIP
	};

	Result ApplyFix(AnimData* animData, BSAnimGroupSequence* destAnim);
	void ApplyHooks();
}
