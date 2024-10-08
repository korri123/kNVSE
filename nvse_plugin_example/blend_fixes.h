﻿#pragma once
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
	void ApplyAttackToAttackISFix();
	void ApplyAimBlendHooks();
	void FixConflictingPriorities(BSAnimGroupSequence* pkSource, BSAnimGroupSequence* pkDest, BSAnimGroupSequence* pkIdle);
	void ApplyHooks();
	void FixPrematureFirstPersonEnd(AnimData* animData, BSAnimGroupSequence* anim);
}

extern std::unordered_map<NiControllerManager*, std::array<NiControllerSequence*, 8>> g_tempBlendSequences;