#include "aim_transitions.h"

#include <algorithm>

#include "anim_switch.h"
#include "class_vtbls.h"
#include "commands_animation.h"
#include "GameObjects.h"
#include "hooks.h"
#include "SafeWrite.h"

namespace
{
	// How close to the end it heads for an aim transition counts as being there
	constexpr float kEndTolerance = 1e-4f;

	bool IsEnabled()
	{
		return g_pluginSettings.blendSmoothing && g_pluginSettings.fixSpineBlendBug && g_pluginSettings.aimTransitions;
	}

	// How AimTransitions plays a sequence (a clip). None for any other sequence. This is a copy of what the
	// SequenceExtraData says, because that data only stays until another sequence of the skeleton gets data (the
	// eases of a switch).
	struct Playback
	{
		TransitionKind kind = TransitionKind::None;
		// Its time runs backwards
		bool bReversed = false;

		explicit operator bool() const { return kind != TransitionKind::None; }
		// Returns true if it takes the weapon to the sights: a start animation played forward, or a stop animation back
		bool HeadsToSights() const { return (kind == TransitionKind::Start) != bReversed; }
	};

	Playback GetPlayback(const NiControllerSequence* sequence)
	{
		const auto* extraData = sequence ? SequenceExtraDatas::Get(sequence) : nullptr;
		if (!extraData || !extraData->bAimTransition)
			return {};
		return { extraData->transition, extraData->bAimReversed };
	}

	// Returns where Animation::Update ends the clip: its End key (TransitionAnims::Prepare made it valid)
	float GetEndTime(const BSAnimGroupSequence* clip)
	{
		const auto& keyTimes = clip->animGroup->keyTimes;
		return keyTimes.Size() >= 2 ? keyTimes[1] : clip->m_fEndKeyTime;
	}

	// Returns the clip's time as Animation::Update counts it (Animation::GetDuration: offset + the Animation's time),
	// which decides where it ends. The sequence time is the same (SetTime, frequency 1) except past either end.
	float GetTime(const AnimData* animData, const BSAnimGroupSequence* clip)
	{
		if (clip->m_fOffset == -NI_INFINITY)
			return clip->m_fBeginKeyTime;
		return clip->m_fOffset + animData->timePassed;
	}

	// Returns the clip's time within its length (held at an end, its time runs on past it)
	float GetClampedTime(const AnimData* animData, const BSAnimGroupSequence* clip)
	{
		return std::clamp(GetTime(animData, clip), clip->m_fBeginKeyTime, GetEndTime(clip));
	}

	// Returns how far towards the sights the clip has taken the weapon, from 0 (hip) to 1
	float GetSightsProgress(const AnimData* animData, const BSAnimGroupSequence* clip, Playback playback)
	{
		const float fBegin = clip->m_fBeginKeyTime;
		const float fEnd = GetEndTime(clip);
		const float fFraction = fEnd > fBegin ? (GetClampedTime(animData, clip) - fBegin) / (fEnd - fBegin) : 1.0f;
		return playback.kind == TransitionKind::Start ? fFraction : 1.0f - fFraction;
	}

	// Returns true if the clip reached the end it heads for: its End key, or its beginning when played back
	bool IsAtEnd(const AnimData* animData, const BSAnimGroupSequence* clip, Playback playback)
	{
		const float fTime = GetTime(animData, clip);
		return playback.bReversed ? fTime <= clip->m_fBeginKeyTime + kEndTolerance : fTime >= GetEndTime(clip) - kEndTolerance;
	}

	// Puts the clip at fTime in the Animation's time, so its next update continues from there. Animation::Update
	// counts time by the offset (GetTime). NiControllerSequence::ComputeScaledTime uses the weighted time it reached
	// and the change of the offset time since its last update.
	void SetTime(const AnimData* animData, BSAnimGroupSequence* clip, float fTime)
	{
		clip->m_fOffset = fTime - animData->timePassed;
		clip->m_fLastTime = fTime;
		clip->m_fWeightedLastTime = fTime;
		clip->m_fLastScaledTime = fTime;
	}

	// Makes the sequence the weapon section's current one under groupId
	void MakeCurrent(AnimData* animData, BSAnimGroupSequence* sequence, UInt16 groupId)
	{
		animData->animSequence[kSequence_Weapon] = sequence;
		animData->groupIDs[kSequence_Weapon] = groupId;
		animData->sequenceState1[kSequence_Weapon] = 0;
		// Third person: the sequence the engine weights by the aim pitch with its up and down variants
		// (MiddleHighProcess::UpdateAimSequences; AimPitch holds those at 0 for an aim transition)
		if (animData != PlayerCharacter::GetSingleton()->firstPersonAnimData)
		{
			if (auto* process = animData->actor->baseProcess; IS_TYPE(process, HighProcess))
				process->weaponSequence[0] = sequence;
		}
	}

	// Picks one of the aimIS group's aim transitions of the kind at random
	BSAnimGroupSequence* Pick(AnimData* animData, UInt16 aimISGroupId, TransitionKind kind)
	{
		auto* set = TransitionAnims::FindSet(animData, aimISGroupId, kind);
		if (!set)
			return nullptr;
		auto paths = TransitionAnims::GetPaths(*set, kind);
		while (!paths.empty())
		{
			const auto index = GetRandomUInt(paths.size());
			if (auto* sequence = Overrides::Load(*set, *paths[index], animData).sequence; sequence && sequence->animGroup)
				return sequence;
			paths.erase(paths.begin() + index);
		}
		return nullptr;
	}

	// Returns true if the aim is easing in from the aim of the other base group (aim from aimIS or the reverse, switched
	// to without an aim transition). The weapon is then on its way between the sights and the hip by that ease. When
	// easing in from anything else (an attack, the draw, the other stance, an aim transition at its end), the weapon is
	// where this aim has it.
	bool IsEasingInFromAim(const BSAnimGroupSequence* aim, UInt32 otherBaseGroupId)
	{
		const UInt32 uiSwitch = aim->m_eState == NiControllerSequence::EASEIN ? AnimSwitch::Of(aim) : 0;
		if (!uiSwitch || !aim->m_pkOwner)
			return false;
		for (const auto* active : aim->m_pkOwner->m_kActiveSequences)
		{
			const auto* other = static_cast<const BSAnimGroupSequence*>(active);
			if (!other || other->m_eState != NiControllerSequence::EASEOUT || NOT_TYPE(other, BSAnimGroupSequence) || !other->animGroup)
				continue;
			if (other->animGroup->GetBaseGroupID() == otherBaseGroupId && AnimSwitch::Of(other) == uiSwitch && !GetPlayback(other))
				return true;
		}
		return false;
	}

	// Plays the clip in the weapon section under groupId from fFraction of its length, cross fading from the sequence
	// that plays there over its blend in. A clip still fading out (switched away from moments ago) eases back in from
	// the weight and time it has, forward. Moving it to fFraction would show a jump from the weight it still has.
	// Returns false, with nothing changed, if it cannot start.
	bool Play(AnimData* animData, BSAnimGroupSequence* clip, TransitionKind kind, BSAnimGroupSequence* from,
		float fFraction, UInt16 groupId)
	{
		// What AnimSwitch::EaseIn starts
		const bool bResume = clip->m_eState == NiControllerSequence::EASEOUT;
		if (!bResume && clip->m_eState != NiControllerSequence::INACTIVE)
			return false;
		auto* extraData = SequenceExtraDatas::GetOrCreate(clip);
		if (!extraData)
			return false;
		const float fBegin = clip->m_fBeginKeyTime;
		const float fTime = bResume ? GetClampedTime(animData, clip) : fBegin + fFraction * (GetEndTime(clip) - fBegin);
		// Set before the clip attaches its interpolators: AimPitch gives its spine bones their shares then
		extraData->transition = kind;
		extraData->bAimTransition = true;
		extraData->bAimReversed = false;
		const float fBlend = clip->GetEaseInTime();
		const auto animSwitch = AnimSwitch::Current(animData);
		animSwitch.EaseOut(from, fBlend);
		if (!animSwitch.EaseIn(clip, fBlend))
			return false;
		SetTime(animData, clip, fTime);
		// Its text keys act from that time on, those before it are behind it
		HandleExtraOperations(animData, clip);
		MakeCurrent(animData, clip, groupId);
		return true;
	}

	// Plays the clip back from where it is, or forward again
	void Reverse(AnimData* animData, BSAnimGroupSequence* clip, UInt16 groupId)
	{
		auto* extraData = SequenceExtraDatas::Get(clip);
		if (!extraData)
			return;
		const bool bReversed = !extraData->bAimReversed;
		extraData->bAimReversed = bReversed;
		SetTime(animData, clip, GetClampedTime(animData, clip));
		// Forward again: its text keys act from that time on (played back they don't, see IsPlayedBack)
		if (!bReversed)
			HandleExtraOperations(animData, clip);
		animData->groupIDs[kSequence_Weapon] = groupId;
	}

	// Makes the clip head for the sights or the hip under groupId, the aim the engine asked for. A clip that already
	// heads there plays on. Otherwise the aim transition for that way plays from the point the weapon has reached, or
	// the clip plays back when there is none. Returns the sequence that plays.
	BSAnimGroupSequence* Steer(AnimData* animData, BSAnimGroupSequence* clip, Playback playback, bool bToSights, UInt16 groupId)
	{
		if (playback.HeadsToSights() == bToSights)
		{
			// The aim asked for can differ from the one the clip started under (the other stance's)
			animData->groupIDs[kSequence_Weapon] = groupId;
			return clip;
		}
		const float fProgress = GetSightsProgress(animData, clip, playback);
		const auto kind = bToSights ? TransitionKind::Start : TransitionKind::Stop;
		if (auto* other = Pick(animData, clip->animGroup->groupID, kind); other && other != clip
			&& Play(animData, other, kind, clip, bToSights ? fProgress : 1.0f - fProgress, groupId))
		{
			return other;
		}
		Reverse(animData, clip, groupId);
		return clip;
	}

	// Returns the player's skeleton that the manager animates
	AnimData* GetPlayerAnimData(const NiControllerManager* manager)
	{
		if (!manager)
			return nullptr;
		for (auto* animData : { PlayerCharacter::GetSingleton()->GetAnimData(false), PlayerCharacter::GetSingleton()->GetAnimData(true) })
		{
			if (animData && animData->controllerManager == manager)
				return animData;
		}
		return nullptr;
	}

	// AnimSequenceMultiple::GetCorrespondingSequence in Animation::StartGroup (0x4948C0) and Animation::SyncSequences.
	// The up or down variant of a group that has several KFs is the one named after the weapon section's sequence
	// (2haaim.kf: 2haaimup.kf). An aim transition stands in for the aim the engine plays (the section's group), so the
	// variant is looked up from that aim. Otherwise releasing aim during a start animation would leave the third person
	// aim without up and down variants.
	BSAnimGroupSequence* __fastcall CorrespondingSequenceHook(void* group, void*, BSAnimGroupSequence* sequence, int eLayer)
	{
		if (GetPlayback(sequence))
		{
			if (auto* animData = GetPlayerAnimData(sequence->m_pkOwner))
			{
				auto* aim = GetAnimByFullGroupID(animData, animData->groupIDs[kSequence_Weapon]);
				if (aim && !GetPlayback(aim))
					sequence = aim;
			}
		}
		return ThisStdCall<BSAnimGroupSequence*>(0x48F500, group, sequence, eLayer);
	}

	// NiControllerSequence::GetState of the weapon section's current sequence in Actor::FiresWeapon's check that the aim
	// and its up and down variants have blended in (Stewie's Tweaks bFireWhileAiming skips the check for the player
	// outside VATS). The engine checks the player's third person skeleton (Actor::GetAnimation) also in first person,
	// where the first person one shows the aim, so this uses the player's skeleton of the view.
	NiControllerSequence::AnimState __fastcall FireAimStateHook(const NiControllerSequence* sequence)
	{
		const NiControllerSequence* shown = sequence;
		if (GetPlayerAnimData(sequence->m_pkOwner))
		{
			const auto* view = PlayerCharacter::GetSingleton()->GetAnimData();
			shown = view ? view->animSequence[kSequence_Weapon] : nullptr;
		}
		if (AimTransitions::HoldsFire(shown))
			return NiControllerSequence::EASEIN;
		return sequence->m_eState;
	}
}

TransitionKind AimTransitions::Of(const NiControllerSequence* sequence)
{
	return GetPlayback(sequence).kind;
}

bool AimTransitions::IsPlayedBack(const NiControllerSequence* sequence)
{
	return GetPlayback(sequence).bReversed;
}

bool AimTransitions::HoldsFire(const NiControllerSequence* sequence)
{
	return IsEnabled() && GetPlayback(sequence);
}

BSAnimGroupSequence* AimTransitions::Switch(AnimData* animData, BSAnimGroupSequence* to)
{
	if (!IsEnabled() || animData->actor != PlayerCharacter::GetSingleton() || !to || !to->animGroup)
		return nullptr;
	const auto toBaseGroupId = to->animGroup->GetBaseGroupID();
	if (toBaseGroupId != kAnimGroup_Aim && toBaseGroupId != kAnimGroup_AimIS)
		return nullptr;
	auto* from = animData->animSequence[kSequence_Weapon];
	if (!from || !from->animGroup || from == to)
		return nullptr;
	const bool bToSights = toBaseGroupId == kAnimGroup_AimIS;
	const UInt16 groupId = to->animGroup->groupID;

	if (const Playback playback = GetPlayback(from))
	{
		// At the end it headed for, in the pose of that aim: switch to it as from any other sequence
		if (playback.HeadsToSights() == bToSights && IsAtEnd(animData, from, playback))
			return nullptr;
		return Steer(animData, from, playback, bToSights, groupId);
	}

	// From the other aim
	if (from->animGroup->GetBaseGroupID() != (bToSights ? kAnimGroup_Aim : kAnimGroup_AimIS))
		return nullptr;
	const auto kind = bToSights ? TransitionKind::Start : TransitionKind::Stop;
	auto* clip = Pick(animData, bToSights ? groupId : from->animGroup->groupID, kind);
	if (!clip)
		return nullptr;
	// An aim still easing in from the one asked for has taken the weapon only that far along its way
	const float fFraction = IsEasingInFromAim(from, toBaseGroupId) ? 1.0f - from->GetEaseSpinner() : 0.0f;
	return Play(animData, clip, kind, from, fFraction, groupId) ? clip : nullptr;
}

void AimTransitions::Update()
{
	if (!IsEnabled() || !PlayerCharacter::GetSingleton() || !PlayerCharacter::GetSingleton()->baseProcess)
		return;
	const bool bAiming = PlayerCharacter::GetSingleton()->GetHighProcess()->isAiming;
	for (auto* animData : { PlayerCharacter::GetSingleton()->GetAnimData(false), PlayerCharacter::GetSingleton()->GetAnimData(true) })
	{
		auto* clip = animData ? animData->animSequence[kSequence_Weapon] : nullptr;
		const Playback playback = GetPlayback(clip);
		if (!playback || clip->m_eState == NiControllerSequence::INACTIVE || clip->m_eState == NiControllerSequence::EASEOUT)
			continue;
		// The engine asks for the aim it wants when that is not the group of the section's sequence
		// (Actor::SetIronSights). A clip is of the aimIS group whichever way it heads, so the engine does not ask when:
		// - aim is pressed while a clip heads to the hip
		// - a clip played back reaches its beginning, in the pose of the aim it came from (a clip played forward is
		//   ended by the engine at its End key in Animation::Update)
		// Animation::BlendOut of the weapon section has Actor::StartWeaponGroup play the aim the engine wants and its up
		// and down variants at once, which ends up in Switch.
		if (playback.HeadsToSights() != bAiming || (playback.bReversed && IsAtEnd(animData, clip, playback)))
			animData->BlendOut(kSequence_Weapon, false);
	}
}

// The engine calls this (SectionClockHook) after the step moved the Animation's time forward. Subtracting twice the
// step from the offset runs the clip's time (GetTime) backwards at the speed it plays forward. Nothing else moves the
// offset: the aim groups are in none of the ranges whose clock Animation::Update runs at an actor speed (movement,
// equip, attack, reload). Animation::Update doesn't call it while it holds the weapon sections
// (Animation::cSkipUpdate): it then takes the step from the section's sequence itself, and a clip fading out beside
// that one runs forward.
void AimTransitions::RunClock(AnimData* animData, const NiControllerSequence* sequence, float fStep)
{
	if (!IsEnabled() || animData->actor != PlayerCharacter::GetSingleton() || sequence != animData->animSequence[kSequence_Weapon] || !sequence->m_pkOwner)
		return;
	// Includes clips that are fading out
	for (auto* active : sequence->m_pkOwner->m_kActiveSequences)
	{
		if (active && active->m_fOffset != -NI_INFINITY && GetPlayback(active).bReversed)
			active->m_fOffset -= 2.0f * fStep;
	}
}

void AimTransitions::WriteHooks()
{
	// Animation::StartGroup, Animation::SyncSequences: the two calls of 0x48F500, which UpDownVariantFix also replaces
	WriteRelCall(0x49495D, CorrespondingSequenceHook);
	WriteRelCall(0x495E0A, CorrespondingSequenceHook);
}

void AimTransitions::WriteDelayedHooks()
{
	// Actor::FiresWeapon
	WriteRelCall(0x894388, FireAimStateHook);
}
