#include "transition_anims.h"

#include <algorithm>
#include <atomic>

#include "commands_animation.h"
#include "sequence_extradata.h"
#include "string_view_util.h"

namespace
{
	TransitionKind GetKind(const AnimPath& path)
	{
		return path.isStartAnim ? TransitionKind::Start : path.isStopAnim ? TransitionKind::Stop : TransitionKind::None;
	}

	// set from the threads registering override files (TagPath), read by the animation threads
	std::atomic<bool> s_hasGaitStops = false;
}

bool TransitionAnims::IsGait(UInt16 groupId)
{
	const auto baseGroupId = groupId & 0xFF;
	return baseGroupId >= kAnimGroup_Forward && baseGroupId <= kAnimGroup_FastRight;
}

bool TransitionAnims::IsAimIS(UInt16 groupId)
{
	return (groupId & 0xFF) == kAnimGroup_AimIS;
}

TransitionKind TransitionAnims::GetPathKind(std::string_view path, UInt16 groupId)
{
	if (!IsGait(groupId) && !IsAimIS(groupId))
		return TransitionKind::None;
	const auto stem = sv::get_file_stem(path);
	if (sv::contains_ci(stem, "_start"))
		return TransitionKind::Start;
	if (sv::contains_ci(stem, "_stop"))
		return TransitionKind::Stop;
	return TransitionKind::None;
}

void TransitionAnims::TagPath(SavedAnims& set, AnimPath& animPath, UInt16 groupId)
{
	switch (GetPathKind(animPath.path, groupId))
	{
	case TransitionKind::Start:
		animPath.isStartAnim = true;
		set.hasTransitionAnims = true;
		break;
	case TransitionKind::Stop:
		animPath.isStopAnim = true;
		set.hasTransitionAnims = true;
		if (IsGait(groupId))
			s_hasGaitStops.store(true, std::memory_order_relaxed);
		break;
	default:
		break;
	}
}

bool TransitionAnims::HasGaitStops()
{
	return s_hasGaitStops.load(std::memory_order_relaxed);
}

bool TransitionAnims::HasPaths(const SavedAnims& set, TransitionKind kind)
{
	return std::ranges::any_of(set.anims, [&](const auto& path) { return GetKind(*path) == kind; });
}

SavedAnims* TransitionAnims::FindSet(AnimData* animData, UInt16 groupId, TransitionKind kind)
{
	const auto find = [&](TransitionKind ofKind) -> SavedAnims*
	{
		if (const auto baseResult = GetActorAnimation(groupId & 0xFF, animData, ofKind); baseResult && baseResult->animBundle->matchBaseGroupId)
			return baseResult->animBundle;
		if (const auto result = GetActorAnimation(groupId, animData, ofKind))
			return result->animBundle;
		return nullptr;
	};
	auto* set = find(kind);
	if (set && HasPaths(*set, TransitionKind::None) && set != find(TransitionKind::None))
		return nullptr;
	return set;
}

std::vector<const AnimPath*> TransitionAnims::GetPaths(SavedAnims& set, TransitionKind kind)
{
	std::vector<const AnimPath*> paths;
	if (!set.hasTransitionAnims)
		return paths;
	for (const auto& path : set.anims)
	{
		if (GetKind(*path) == kind)
			paths.push_back(path.get());
	}
	return paths;
}

void TransitionAnims::Prepare(BSAnimGroupSequence* sequence)
{
	if (!sequence->animGroup)
		return;
	auto& keyTimes = sequence->animGroup->keyTimes;
	if (keyTimes.Size() >= 2 && !(keyTimes[1] > keyTimes[0]))
		keyTimes[1] = sequence->m_fEndKeyTime;
	if (IsAimIS(sequence->animGroup->groupID))
		sequence->m_eCycleType = NiControllerSequence::CLAMP;
}

TransitionKind TransitionAnims::Of(const NiControllerSequence* sequence)
{
	if (!sequence)
		return TransitionKind::None;
	const auto* extraData = SequenceExtraDatas::Get(sequence);
	return extraData ? extraData->transition : TransitionKind::None;
}
