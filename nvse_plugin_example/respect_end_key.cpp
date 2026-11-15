#include "respect_end_key.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

#include "SafeWrite.h"
#include "utility.h"

namespace
{
	struct Swap
	{
		NiPointer<BSAnimGroupSequence> anim3rd;
		// whose times the copy has; its text keys answer the a: key lookup (AnimData::GetNextWeaponSequenceKey hook)
		NiPointer<BSAnimGroupSequence> anim1st;
		NiPointer<TESAnimGroup> original;
		NiPointer<TESAnimGroup> copy;
		// applied or kept since the last EndFrame
		bool bClaimed = false;
	};

	// The player's 3rd person sequences that have a copy: one or two at a time. Changed on the main thread, read by
	// Animation::AddAnimation (any Animation, on AI and loader threads too) and the a: key lookup
	std::vector<Swap> g_swaps;
	std::mutex g_mutex;
	std::atomic<bool> g_bAnySwaps = false;

	Swap* Find(const BSAnimGroupSequence* anim3rd)
	{
		for (auto& swap : g_swaps)
		{
			if (swap.anim3rd == anim3rd)
				return &swap;
		}
		return nullptr;
	}

	void CopyTimes(TESAnimGroup& to, const TESAnimGroup& from)
	{
		to.keyTimes = from.keyTimes;
		to.blend = from.blend;
		to.blendIn = from.blendIn;
		to.blendOut = from.blendOut;
	}

	// A copy of original with the times of times. The game's TESAnimGroup has no copy constructor: its fields as they
	// are (vtable, group ID, move vector, the prn: node name, which points into the KF's text keys and is not freed by
	// ~TESAnimGroup), its own key time and sound arrays, which ~TESAnimGroup frees
	TESAnimGroup* MakeCopy(const TESAnimGroup& original, const TESAnimGroup& times)
	{
		auto* copy = NiNew<TESAnimGroup>();
		std::memcpy(static_cast<void*>(copy), &original, sizeof(TESAnimGroup));
		copy->m_uiRefCount = 0;
		new (&copy->keyTimes) SimpleFixedArray<float>(times.keyTimes);
		new (&copy->sounds) SimpleFixedArray<TESAnimGroup::AnimGroupSound>(original.sounds);
		copy->blend = times.blend;
		copy->blendIn = times.blendIn;
		copy->blendOut = times.blendOut;
		return copy;
	}

	// Animation::AddAnimation takes a KF it already has when its sequence's anim group is the KF model's
	// (BSAnimGroupSequence::GetAnimGroup at 0x490768): compare the group before a copy, or it would replace the sequence
	TESAnimGroup* __fastcall GetLoadedSequenceGroupHook(BSAnimGroupSequence* sequence)
	{
		return RespectEndKey::OriginalGroup(sequence);
	}
}

void RespectEndKey::Apply(BSAnimGroupSequence* anim3rd, BSAnimGroupSequence* anim1st)
{
	TESAnimGroup* times = anim1st ? anim1st->animGroup.data : nullptr;
	TESAnimGroup* current = anim3rd ? anim3rd->animGroup.data : nullptr;
	if (!times || !current)
		return;
	std::scoped_lock lock(g_mutex);
	Swap* swap = Find(anim3rd);
	if (swap && current == swap->copy)
	{
		// the copy is this sequence's alone: changed in place, so no code of the engine's is left with a freed group
		if (swap->anim1st != anim1st)
		{
			CopyTimes(*swap->copy, *times);
			swap->anim1st = anim1st;
		}
		swap->bClaimed = true;
		return;
	}
	// a group given to the sequence since (SetAnimTextKeys) is its own one now
	const NiPointer<TESAnimGroup> original = current;
	const NiPointer<TESAnimGroup> copy = MakeCopy(*original, *times);
	if (!swap)
	{
		swap = &g_swaps.emplace_back();
		swap->anim3rd = anim3rd;
	}
	swap->anim1st = anim1st;
	swap->original = original;
	swap->copy = copy;
	swap->bClaimed = true;
	anim3rd->animGroup = copy;
	g_bAnySwaps = true;
}

void RespectEndKey::Keep(BSAnimGroupSequence* anim3rd)
{
	if (!g_bAnySwaps)
		return;
	std::scoped_lock lock(g_mutex);
	if (Swap* swap = Find(anim3rd); swap && anim3rd->animGroup == swap->copy)
		swap->bClaimed = true;
}

void RespectEndKey::EndFrame()
{
	if (!g_bAnySwaps)
		return;
	std::scoped_lock lock(g_mutex);
	std::erase_if(g_swaps, [](Swap& swap)
	{
		if (swap.bClaimed)
		{
			swap.bClaimed = false;
			return false;
		}
		if (swap.anim3rd->animGroup == swap.copy)
			swap.anim3rd->animGroup = swap.original;
		return true;
	});
	g_bAnySwaps = !g_swaps.empty();
}

BSAnimGroupSequence* RespectEndKey::TimesSource(BSAnimGroupSequence* anim3rd)
{
	if (!g_bAnySwaps || !anim3rd)
		return nullptr;
	std::scoped_lock lock(g_mutex);
	const Swap* swap = Find(anim3rd);
	return swap && anim3rd->animGroup == swap->copy ? swap->anim1st.data : nullptr;
}

TESAnimGroup* RespectEndKey::OriginalGroup(BSAnimGroupSequence* sequence)
{
	if (!g_bAnySwaps)
		return sequence->animGroup;
	std::scoped_lock lock(g_mutex);
	const Swap* swap = Find(sequence);
	return swap && sequence->animGroup == swap->copy ? swap->original.data : sequence->animGroup.data;
}

void RespectEndKey::WriteDelayedHooks()
{
	// Animation::AddAnimation: BSAnimGroupSequence::GetAnimGroup of the sequence the Animation has for the group
	WriteRelCall(0x490768, GetLoadedSequenceGroupHook);
}
