#pragma once
#include <unordered_set>

#include "commands_animation.h"

struct AnimData;
class BSAnimGroupSequence;
enum AnimGroupID : UInt8;

extern std::unordered_set<std::string> g_reloadStartBlendFixes;

struct AnimationContext
{
	UInt8* basePointer = nullptr;

	AnimationContext() = default;

	explicit AnimationContext(UInt8* basePointer):
		basePointer(basePointer),
		groupID(reinterpret_cast<UInt32*>(basePointer + 0xC)),
		animData(*reinterpret_cast<AnimData**>(basePointer - 0x5C)),
		blendAmount(reinterpret_cast<float*>(basePointer - 0x34)),
		toAnim(reinterpret_cast<BSAnimGroupSequence**>(basePointer + 0x8)),
		fromAnim(reinterpret_cast<BSAnimGroupSequence**>(basePointer - 0x20)),
		sequenceId(reinterpret_cast<eAnimSequence*>(basePointer + 0x10))
	{
	}
	UInt32* groupID = nullptr;
	AnimData* animData = nullptr;
	float* blendAmount = nullptr;
	BSAnimGroupSequence** toAnim = nullptr;
	BSAnimGroupSequence** fromAnim = nullptr;
	eAnimSequence* sequenceId = nullptr;
};

extern AnimationContext g_animationHookContext;
extern bool g_startedAnimation;
extern BSAnimGroupSequence* g_lastLoopSequence;
void ApplyHooks();
extern bool g_doNotSwapAnims;