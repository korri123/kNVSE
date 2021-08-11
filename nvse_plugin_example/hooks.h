#pragma once
#include <unordered_set>
struct AnimData;
class BSAnimGroupSequence;
enum AnimGroupID : UInt8;

extern std::unordered_set<BSAnimGroupSequence*> g_reloadStartBlendFixes;

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
		fromAnim(reinterpret_cast<BSAnimGroupSequence**>(basePointer - 0x20))
	{
	}
	UInt32* groupID = nullptr;
	AnimData* animData = nullptr;
	float* blendAmount = nullptr;
	BSAnimGroupSequence** toAnim = nullptr;
	BSAnimGroupSequence** fromAnim = nullptr;
};

extern AnimationContext g_animationHookContext;
extern bool g_startedAnimation;
extern BSAnimGroupSequence* g_lastLoopSequence;
void ApplyHooks();