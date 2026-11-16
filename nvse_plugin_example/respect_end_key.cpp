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
		// The 1st person animation whose times the copy has. Its text keys answer the a: key lookup
		// (AnimData::GetNextWeaponSequenceKey hook).
		NiPointer<BSAnimGroupSequence> anim1st;
		NiPointer<TESAnimGroup> original;
		NiPointer<TESAnimGroup> copy;
		// Applied or kept since the last EndFrame
		bool bClaimed = false;
	};

	// The player's 3rd person sequences that have a copy (one or two at a time). Changed on the main thread, read by
	// Animation::AddAnimation (any Animation, including AI and loader threads) and the a: key lookup.
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

	// Returns a copy of original with the times of `times`. The game's TESAnimGroup has no copy constructor, so the
	// fields are copied as they are (vtable, group ID, move vector, the prn: node name, which points into the KF's text
	// keys and is not freed by ~TESAnimGroup). The key time and sound arrays are the copy's own and ~TESAnimGroup frees
	// them.
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

	// Animation::AddAnimation reuses a KF it already has when its sequence's anim group is the KF model's
	// (BSAnimGroupSequence::GetAnimGroup at 0x490768). Returns the original group, so the check still matches while
	// a copy is applied and doesn't replace the sequence.
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
		// The copy belongs to this sequence alone and is changed in place, so no engine code is left with a freed group
		if (swap->anim1st != anim1st)
		{
			CopyTimes(*swap->copy, *times);
			swap->anim1st = anim1st;
		}
		swap->bClaimed = true;
		return;
	}
	// A group given to the sequence since then (SetAnimTextKeys) is its own now
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
