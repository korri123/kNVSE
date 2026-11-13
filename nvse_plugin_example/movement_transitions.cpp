#include "movement_transitions.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#include "blend_fixes.h"
#include "class_vtbls.h"
#include "commands_animation.h"
#include "hooks.h"
#include "movement_blend_fixes.h"
#include "SafeWrite.h"
#include "string_view_util.h"

namespace
{
	// A stop animation played this far (of its length): the actor counts as standing, so walking again starts with the
	// start animation instead of blending back to the walk
	constexpr float kStandingProgress = 0.5f;

	bool IsEnabled()
	{
		return g_pluginSettings.blendSmoothing && g_pluginSettings.movementTransitions;
	}

	// Forward ... FastRight: the groups Actor::PickAnimations plays while the actor walks or runs
	bool IsGait(UInt16 groupId)
	{
		const auto baseGroupId = groupId & 0xFF;
		return baseGroupId >= kAnimGroup_Forward && baseGroupId <= kAnimGroup_FastRight;
	}

	bool IsGait(const BSAnimGroupSequence* sequence)
	{
		return sequence && sequence->animGroup && IsGait(sequence->animGroup->groupID);
	}

	MovementTransition GetTransition(const AnimPath& path)
	{
		return path.isStartAnim ? MovementTransition::Start : path.isStopAnim ? MovementTransition::Stop : MovementTransition::None;
	}

	// The override set a walk or run group's start or stop animations come from: the one HandleAnimationChange takes the
	// group's animation from, or one without an animation of the group (start and stop animations for whatever walk
	// plays). Those of a set whose walk another set overrides stay with that walk
	SavedAnims* FindOverrideSet(AnimData* animData, UInt16 groupId, MovementTransition transition)
	{
		const auto find = [&](MovementTransition kind) -> SavedAnims*
		{
			if (const auto baseResult = GetActorAnimation(groupId & 0xFF, animData, kind); baseResult && baseResult->animBundle->matchBaseGroupId)
				return baseResult->animBundle;
			if (const auto result = GetActorAnimation(groupId, animData, kind))
				return result->animBundle;
			return nullptr;
		};
		auto* set = find(transition);
		if (set && MovementTransitions::HasPaths(*set, MovementTransition::None) && set != find(MovementTransition::None))
			return nullptr;
		return set;
	}

	std::vector<const AnimPath*> GetTransitionPaths(SavedAnims& set, MovementTransition transition)
	{
		std::vector<const AnimPath*> paths;
		if (!set.hasMovementTransitions)
			return paths;
		for (const auto& path : set.anims)
		{
			if (GetTransition(*path) == transition)
				paths.push_back(path.get());
		}
		return paths;
	}

	// Start and stop animations and what blends with them in the movement section play at fMoveSpeed (ClockStateHook), so
	// blends into and out of them take their ease time in the animations' time
	float ToRealTime(const AnimData* animData, float fAnimTime)
	{
		const float fSpeed = animData->movementSpeedMult;
		return fSpeed > 0.0f && std::isfinite(fSpeed) ? fAnimTime / fSpeed : fAnimTime;
	}

	// Marks the sequence and plays it in the movement section under groupId (the group the engine picked, which it keeps
	// testing the section against), cross fading from what plays there. False if it could not start
	bool Play(AnimData* animData, BSAnimGroupSequence* sequence, UInt16 groupId, MovementTransition transition)
	{
		auto* extraData = SequenceExtraDatas::GetOrCreate(sequence);
		if (!extraData)
			return false;
		extraData->movementTransition = transition;
		HandleExtraOperations(animData, sequence);
		MovementBlendFixes::PlayMovementAnim(animData, sequence, ToRealTime(animData, sequence->GetEaseInTime()));
		if (sequence->m_eState == NiControllerSequence::INACTIVE)
			return false;
		animData->groupIDs[kSequence_Movement] = groupId;
		return true;
	}

	float GetWeight(const NiControllerSequence* sequence)
	{
		switch (sequence->m_eState)
		{
		case NiControllerSequence::INACTIVE:
			return 0.0f;
		case NiControllerSequence::EASEIN:
		case NiControllerSequence::EASEOUT:
			return sequence->m_fSeqWeight * sequence->GetEaseSpinner();
		default:
			return sequence->m_fSeqWeight;
		}
	}

	// How far the sequence has played, from 0 to 1. A sequence started over (m_fOffset unset until its first update)
	// still has the times of its previous play
	float GetProgress(const NiControllerSequence* sequence)
	{
		const float fLength = sequence->m_fEndKeyTime - sequence->m_fBeginKeyTime;
		if (sequence->m_fOffset == -NI_INFINITY || sequence->m_fLastScaledTime == -NI_INFINITY || fLength <= 0.0f)
			return 0.0f;
		return std::clamp((sequence->m_fLastScaledTime - sequence->m_fBeginKeyTime) / fLength, 0.0f, 1.0f);
	}

	bool IsStopNearlyDone(const NiControllerSequence* sequence)
	{
		return MovementTransitions::Of(sequence) == MovementTransition::Stop && GetProgress(sequence) >= kStandingProgress;
	}

	// Whether walking or running would start from standing: the movement section is empty, turning in place or most of
	// the way through a stop animation, and no walk or run (or start or stop animation) still fading out has most of the
	// legs (stopping and walking on at once, landing a jump)
	bool IsStanding(AnimData* animData)
	{
		if (const auto* current = animData->animSequence[kSequence_Movement])
		{
			const bool bTurning = current->animGroup && current->animGroup->IsTurning();
			if (!bTurning && !IsStopNearlyDone(current))
				return false;
		}
		for (auto* sequence : animData->controllerManager->m_kActiveSequences)
		{
			const auto* bsSequence = static_cast<const BSAnimGroupSequence*>(sequence);
			if (!bsSequence || NOT_TYPE(bsSequence, BSAnimGroupSequence) || !IsGait(bsSequence) || IsStopNearlyDone(bsSequence))
				continue;
			if (GetWeight(bsSequence) >= 0.5f)
				return false;
		}
		return true;
	}

	bool HasActiveTransition(NiControllerManager* manager)
	{
		if (!manager)
			return false;
		for (const auto* sequence : manager->m_kActiveSequences)
		{
			if (sequence && MovementTransitions::Of(sequence) != MovementTransition::None)
				return true;
		}
		return false;
	}

	// The walk or run the actor is in most: on a stick release the run hands over to a walk for a few frames before the
	// stop, while it still has the legs
	BSAnimGroupSequence* GetMainGait(AnimData* animData)
	{
		BSAnimGroupSequence* result = nullptr;
		float fResultWeight = 0.0f;
		for (auto* sequence : animData->controllerManager->m_kActiveSequences)
		{
			auto* bsSequence = static_cast<BSAnimGroupSequence*>(sequence);
			if (!bsSequence || NOT_TYPE(bsSequence, BSAnimGroupSequence) || !IsGait(bsSequence))
				continue;
			const float fWeight = GetWeight(bsSequence);
			if (!result || fWeight > fResultWeight)
			{
				result = bsSequence;
				fResultWeight = fWeight;
			}
		}
		return result;
	}

	// Animation::BlendOut of the movement section in Actor::PickAnimations, for a group picked in another section (idle):
	// the stop animation of the walk or run instead. False to blend out as the engine does.
	// The player only: an NPC is told to stop where its path ends, and its movement is dropped from then on
	// (ActorMover::SetCurrentFrameMoveModeAndDelta while Animation::ShouldBeMoving), so a stop animation started there
	// could only step in place. NPCs start theirs before they get there (StartArrival)
	bool PlayStop(AnimData* animData)
	{
		auto* current = animData->animSequence[kSequence_Movement];
		if (animData->actor != g_thePlayer || !IsGait(current) || animData->noBlend120)
			return false;
		const auto currentTransition = MovementTransitions::Of(current);
		// already stopping (the GetState hook keeps it), or released while the start animation was still blending in
		if (currentTransition == MovementTransition::Stop || (currentTransition == MovementTransition::Start && current->m_eState == NiControllerSequence::EASEIN))
			return false;
		auto* gait = GetMainGait(animData);
		if (!gait)
			return false;
		const UInt16 groupId = gait == current ? animData->groupIDs[kSequence_Movement] : gait->animGroup->groupID;
		if (!IsGait(groupId))
			return false;
		auto* set = FindOverrideSet(animData, groupId, MovementTransition::Stop);
		if (!set)
			return false;
		const auto paths = GetTransitionPaths(*set, MovementTransition::Stop);
		if (paths.empty())
			return false;

		// the one whose first frame has the feet where they are: stops on the left and on the right foot
		BSAnimGroupSequence* stop = nullptr;
		float fBestMismatch = NI_INFINITY;
		std::vector<BSAnimGroupSequence*> unmatched;
		for (const auto* path : paths)
		{
			const auto animCtx = LoadCustomAnimation(*set, *path, animData);
			auto* sequence = animCtx ? animCtx->anim.data : nullptr;
			if (!sequence)
				continue;
			const float fMismatch = MovementBlendFixes::GetStrideMismatch(animData, sequence, sequence->m_fBeginKeyTime);
			if (std::isnan(fMismatch))
				unmatched.push_back(sequence);
			else if (fMismatch < fBestMismatch)
			{
				stop = sequence;
				fBestMismatch = fMismatch;
			}
		}
		if (!stop && !unmatched.empty())
			stop = unmatched[GetRandomUInt(unmatched.size())];
		if (!stop || !Play(animData, stop, groupId, MovementTransition::Stop))
			return false;
		animData->queuedGroupIDs[kSequence_Movement] = 0xFF;
		return true;
	}

	void __fastcall BlendOutMovementHook(AnimData* animData, void*, eAnimSequence sequenceId, bool bChar)
	{
		if (sequenceId == kSequence_Movement && IsEnabled() && PlayStop(animData))
			return;
		animData->BlendOut(sequenceId, bChar);
	}

	// NPC arrivals. NPCs move by the root motion of their animations (Animation::GetDelta in Actor::GetMovementDelta
	// 0x8B7100), steered along their path by DetailedActorPathHandler::Update (0x9E0A00), and stop once they are within
	// the goal radius of the path's last node (DetailedActorPathHandler 0x9E8000; a soft radius goes on to 5 units).
	// The stop animation starts where its root motion still reaches that point (partway through when the point is
	// nearer than its whole motion), and its root motion is scaled to land there (WarpArrivalHook): measured in
	// Goodsprings, sandboxing NPCs walk paths of 50-300 units to a goal radius of 10, 25 or 50 (soft), so a stop played
	// on arrival would have carried them its whole length past that point
	constexpr float kSoftGoalRadius = 5.0f;
	// how far past the stop point the stop animation aims, so the engine's check passes before its motion runs out
	constexpr float kArrivalOvershoot = 3.0f;
	// root motion scale range: beyond it the feet would visibly slide
	constexpr float kMinWarp = 0.6f;
	constexpr float kMaxWarp = 1.5f;
	// how far short of the stop point an NPC may be when its stop animation's motion runs out (slowed by turns, other
	// actors) to be stopped there: farther, it walks on
	constexpr float kArrivalSlack = 16.0f;
	// how much farther the stop point may get from one update to the next before the stop counts as cancelled (the goal
	// moved on: following, a new path)
	constexpr float kGoalMovedDistance = 2.0f;
	// Whether any override set has a stop animation (TagPath): without one, StartArrival has nothing to start
	std::atomic<bool> s_hasStopAnims = false;

	struct Arrival
	{
		enum class State
		{
			None, // not heading for the end of a path
			Moving,
			Arrived, // stopping or stopped at the path's end (the engine drops the movement), or turning to face its goal
		};
		State state = State::None;
		float fDistance = NAN; // to the point where the NPC stops, while Moving
		NiPoint3 goal; // the path's last node, while Moving
	};

	Arrival GetArrival(Actor* actor)
	{
		auto* mover = actor->actorMover;
		if (!mover || mover->bPathingFailed || mover->bWaitingForPath)
			return {};
		if (mover->bStopMovementNextUpdate || mover->bStopCurrentMoveAnim)
			return { Arrival::State::Arrived };
		auto* handler = mover->pathHandler;
		if (mover->eState != ActorMover::kState_Moving || !handler || *reinterpret_cast<UInt32*>(handler) != kVtbl_DetailedActorPathHandler)
			return {};
		if (handler->eState == DetailedActorPathHandler::kState_GoalReached || handler->bRotating)
			return { Arrival::State::Arrived };
		auto* path = handler->pathingSolution;
		if (handler->eState != DetailedActorPathHandler::kState_Moving || handler->bWaitingForUpdate || handler->bEndUpdatePath ||
			!path || path->bIncompletePath || !path->numCurrentNodes)
			return {};
		// a door the path goes on through
		const auto* goal = path->GetLastNode();
		if (!goal || goal->IsTeleportDoor())
			return {};
		// along the path: to the node it heads for, then node to node
		const UInt32 lastIndex = path->numCurrentNodes - 1;
		const UInt32 nextIndex = (std::min)(handler->currentNodeIndex + 1, lastIndex);
		NiPoint3 from(actor->posX, actor->posY, 0.0f);
		float fDistance = 0.0f;
		for (UInt32 index = nextIndex; index <= lastIndex; ++index)
		{
			const auto* node = index == lastIndex ? goal : path->GetNode(index);
			if (!node)
				return {};
			const NiPoint3& to = node->location.location;
			fDistance += std::hypot(to.x - from.x, to.y - from.y);
			from = to;
		}
		float fStopRadius = std::sqrt(handler->fGoalRadiusSqr);
		if (handler->pathingRequest && handler->pathingRequest->bSoftGoalRadius)
			fStopRadius = (std::min)(fStopRadius, kSoftGoalRadius);
		return { Arrival::State::Moving, (std::max)(fDistance - fStopRadius + kArrivalOvershoot, 0.0f), goal->location.location };
	}

	// Paths are rebuilt on the way (avoidance) and new ones can take a freed one's memory: they are told apart by their goal
	bool IsSameGoal(const NiPoint3& goal, const NiPoint3& other)
	{
		return std::hypot(goal.x - other.x, goal.y - other.y) <= kGoalMovedDistance;
	}

	// A sequence's time as of its last update, its beginning when it is about to start over
	float GetSequenceTime(const NiControllerSequence* sequence)
	{
		if (sequence->m_fOffset == -NI_INFINITY || sequence->m_fLastScaledTime == -NI_INFINITY)
			return sequence->m_fBeginKeyTime;
		return sequence->m_fLastScaledTime;
	}

	// The scale of the actor's movement: its root's (the accum root's parent)
	float GetRootScale(const AnimData* animData)
	{
		const auto* root = animData->nBip01;
		return root && root->m_pkParent ? root->m_pkParent->m_kWorld.m_fScale : 1.0f;
	}

	// The accum root's (Bip01's) track of a sequence: where it moves the actor
	class RootTrack
	{
	public:
		RootTrack(AnimData* animData, const NiControllerSequence* sequence) : sequence(sequence)
		{
			root = animData->nBip01;
			if (!root)
				return;
			if (const auto* block = sequence->GetControlledBlock(root->m_pcName))
				interpolator = block->m_spInterpolator;
			fScale = GetRootScale(animData);
			if (interpolator)
				end = At(sequence->m_fEndKeyTime);
		}

		// interpolators are shared by every clone of a KF and cache their last evaluation by time
		~RootTrack()
		{
			if (interpolator)
				interpolator->m_fLastTime = -NI_INFINITY;
		}

		RootTrack(const RootTrack&) = delete;
		RootTrack& operator=(const RootTrack&) = delete;

		explicit operator bool() const { return interpolator != nullptr; }

		// how far the root moves on the ground from fTime to the sequence's end
		float DistanceToEnd(float fTime) const
		{
			if (!interpolator)
				return 0.0f;
			const NiPoint3 move = end - At(fTime);
			return std::hypot(move.x, move.y) * fScale;
		}

		// the time from which the root moves fDistance to the end: the beginning if it moves less in all
		float TimeAtDistanceToEnd(float fDistance) const
		{
			float fLow = sequence->m_fBeginKeyTime;
			float fHigh = sequence->m_fEndKeyTime;
			if (!interpolator || DistanceToEnd(fLow) <= fDistance)
				return fLow;
			for (int i = 0; i < 16; ++i)
			{
				const float fMid = (fLow + fHigh) * 0.5f;
				(DistanceToEnd(fMid) > fDistance ? fLow : fHigh) = fMid;
			}
			return fHigh;
		}

	private:
		NiPoint3 At(float fTime) const
		{
			NiQuatTransform value;
			if (interpolator->Update(fTime, root, value) && value.IsTranslateValid())
				return value.GetTranslate();
			return root->m_kLocal.m_Translate;
		}

		const NiControllerSequence* sequence;
		NiAVObject* root = nullptr;
		NiInterpolator* interpolator = nullptr;
		float fScale = 1.0f;
		NiPoint3 end{};
	};

	// Starts the stop animation of an NPC's walk or run once its root motion reaches the point where the NPC stops: as
	// soon as that point is as near as the shortest one's motion, each from where it has that much left; of those the one
	// whose pose there has the feet where they are. True if it plays
	bool StartArrival(AnimData* animData)
	{
		const UInt16 groupId = animData->groupIDs[kSequence_Movement];
		// nothing to start: keeps the arrivals (the path handler's state, the lookup of the override set) out of every
		// NPC's update while no stop animation is installed
		if (!s_hasStopAnims.load(std::memory_order_relaxed))
			return false;
		// from a walk or start animation playing on its own: one still blending moves the NPC by less than the stop's
		// motion counts on
		const auto* current = animData->animSequence[kSequence_Movement];
		if (animData->noBlend120 || !IsGait(groupId) || !current || current->m_eState != NiControllerSequence::ANIMATING)
			return false;
		const auto arrival = GetArrival(animData->actor);
		if (arrival.state != Arrival::State::Moving)
			return false;
		auto* set = FindOverrideSet(animData, groupId, MovementTransition::Stop);
		if (!set)
			return false;
		const auto paths = GetTransitionPaths(*set, MovementTransition::Stop);
		if (paths.empty())
			return false;

		// every update while walking: what each has left (the whole motion cached), before anything is sampled
		std::vector<BSAnimGroupSequence*> sequences;
		for (const auto* path : paths)
		{
			const auto animCtx = LoadCustomAnimation(*set, *path, animData);
			auto* sequence = animCtx ? animCtx->anim.data : nullptr;
			auto* extraData = sequence ? SequenceExtraDatas::GetOrCreate(sequence) : nullptr;
			if (!extraData)
				continue;
			// once per approach: after one was cancelled (the NPC fell behind its motion) it walks on to the end
			if (IsSameGoal(extraData->arrivalGoal, arrival.goal) && arrival.fDistance <= extraData->fArrivalDistance + kGoalMovedDistance)
				return false;
			float fLeft;
			if (sequence->m_eState == NiControllerSequence::INACTIVE)
			{
				if (std::isnan(extraData->fRootDistance))
				{
					const RootTrack track(animData, sequence);
					extraData->fRootDistance = track ? track.DistanceToEnd(sequence->m_fBeginKeyTime) / GetRootScale(animData) : 0.0f;
				}
				fLeft = extraData->fRootDistance * GetRootScale(animData);
			}
			else
			{
				const RootTrack track(animData, sequence);
				fLeft = track ? track.DistanceToEnd(GetSequenceTime(sequence)) : 0.0f;
			}
			// no root motion: not one to stop with
			if (fLeft <= 0.0f)
				continue;
			if (arrival.fDistance > fLeft)
				return false;
			sequences.push_back(sequence);
		}

		if (sequences.empty())
			return false;

		struct Candidate
		{
			BSAnimGroupSequence* sequence;
			float fTime;
			bool bStartOver;
			float fMismatch; // of the legs' pose at fTime; infinite if it can't be measured
		};
		std::vector<Candidate> candidates;
		for (auto* sequence : sequences)
		{
			const RootTrack track(animData, sequence);
			// one still easing out (the NPC walked on from it) eases back in where it is: moving its clock would move
			// the root, and with it the NPC, by as much at once
			const bool bStartOver = sequence->m_eState == NiControllerSequence::INACTIVE;
			const float fTime = bStartOver ? track.TimeAtDistanceToEnd(arrival.fDistance) : GetSequenceTime(sequence);
			const float fMismatch = MovementBlendFixes::GetStrideMismatch(animData, sequence, fTime);
			candidates.push_back({ sequence, fTime, bStartOver, std::isnan(fMismatch) ? NI_INFINITY : fMismatch });
		}
		const auto& stop = *std::ranges::min_element(candidates, {}, &Candidate::fMismatch);
		auto* sequence = stop.sequence;
		if (!Play(animData, sequence, groupId, MovementTransition::Stop))
			return false;
		if (stop.bStartOver && stop.fTime > sequence->m_fBeginKeyTime)
		{
			// as MovementBlendFixes::PlayMovementAnim starts a gait partway; the last time skips the text keys before it
			sequence->m_fOffset = stop.fTime / sequence->m_fFrequency - animData->timePassed;
			sequence->m_fLastScaledTime = stop.fTime;
		}
		if (auto* extraData = SequenceExtraDatas::Get(sequence))
		{
			extraData->arrivalGoal = arrival.goal;
			extraData->fArrivalDistance = arrival.fDistance;
		}
		animData->queuedGroupIDs[kSequence_Movement] = 0xFF;
		return true;
	}

	// An NPC's stop animation plays under the walk it stops while the path still moves the NPC: kept unless the goal moves
	// away (following, a new path). When its motion runs out short of the stop point (turns and other actors slow NPCs
	// down), the NPC stops there if it is near, else walks on
	bool KeepArrival(AnimData* animData, BSAnimGroupSequence* stop)
	{
		const auto arrival = GetArrival(animData->actor);
		if (arrival.state != Arrival::State::Moving)
			return arrival.state == Arrival::State::Arrived;
		auto* extraData = SequenceExtraDatas::Get(stop);
		const RootTrack track(animData, stop);
		if (!extraData || !track || !IsSameGoal(arrival.goal, extraData->arrivalGoal) || arrival.fDistance > extraData->fArrivalDistance + kGoalMovedDistance)
			return false;
		extraData->fArrivalDistance = arrival.fDistance;
		if (track.DistanceToEnd(GetSequenceTime(stop)) >= 1.0f)
			return true;
		if (arrival.fDistance > kArrivalSlack)
			return false;
		animData->actor->actorMover->StopMoving();
		return true;
	}

	// NiControllerSequence::GetState of the section's sequence in Actor::PickAnimations (Animation at [ebp-0x4], section at
	// [ebp-0x1C]), deciding whether the group picked is playing already: the stop animation plays under the group it stops,
	// and walking that way again replaces it. NPCs walking a path start their stop animation from here before they arrive
	NiControllerSequence::AnimState __fastcall PickedGroupStateHook(BSAnimGroupSequence* sequence)
	{
		auto* animData = GET_CALLER_VAR(AnimData*, -0x4);
		const bool bArrivals = IsEnabled() && animData->actor != g_thePlayer && GET_CALLER_VAR(UInt32, -0x1C) == kSequence_Movement &&
			sequence == animData->animSequence[kSequence_Movement];
		if (MovementTransitions::Of(sequence) == MovementTransition::Stop)
			return bArrivals && KeepArrival(animData, sequence) ? sequence->m_eState : NiControllerSequence::INACTIVE;
		if (bArrivals && StartArrival(animData))
			return animData->animSequence[kSequence_Movement]->m_eState;
		return sequence->m_eState;
	}

	// Animation::GetDelta in Actor::GetMovementDelta: an NPC's movement along its path for this update. While its stop
	// animation plays, scaled so the animation's root motion ends where the NPC stops
	bool __fastcall WarpArrivalHook(AnimData* animData, void*, NiPoint3* delta, Actor* actor, bool bRotation, bool bNoHeight)
	{
		const bool bResult = animData->GetDelta(delta, actor, bRotation, bNoHeight);
		if (!bResult || !IsEnabled() || actor == g_thePlayer)
			return bResult;
		const auto* stop = animData->animSequence[kSequence_Movement];
		if (MovementTransitions::Of(stop) != MovementTransition::Stop)
			return bResult;
		const auto arrival = GetArrival(actor);
		const RootTrack track(animData, stop);
		if (arrival.state != Arrival::State::Moving || !track)
			return bResult;
		// the delta is the root motion of the last update, which took the animation to its last time
		const float fStep = std::hypot(delta->x, delta->y);
		const float fLeft = fStep + track.DistanceToEnd(GetSequenceTime(stop));
		if (fLeft > 0.01f)
		{
			const float fWarp = std::clamp(arrival.fDistance / fLeft, kMinWarp, kMaxWarp);
			delta->x *= fWarp;
			delta->y *= fWarp;
		}
		return bResult;
	}

	// Animation::ShouldBeMoving in ActorMover::SetCurrentFrameMoveModeAndDelta: once the path has ended, the NPC's
	// movement is dropped while the walk still plays (bStopCurrentMoveAnim), after it Actor::UpdateMovement moves the NPC
	// by the animation's root motion. A stop animation's rest moves it the little it aimed past the stop point
	// (kArrivalOvershoot) instead of stepping in place
	bool __fastcall ArrivedShouldBeMovingHook(AnimData* animData)
	{
		if (IsEnabled() && MovementTransitions::Of(animData->animSequence[kSequence_Movement]) == MovementTransition::Stop)
			return false;
		return animData->ShouldBeMoving();
	}

	// NiControllerSequence::GetState of the movement sequence in Animation::Update: while it eases in (or is TRANSDEST)
	// the actor moves at the group's average speed instead of by the accum root. While a start or stop animation plays or
	// fades, the actor moves by the root motion of what blends in the movement section
	NiControllerSequence::AnimState __fastcall RootMotionStateHook(const NiControllerSequence* sequence)
	{
		const auto eState = sequence->m_eState;
		if ((eState == NiControllerSequence::EASEIN || eState == NiControllerSequence::TRANSDEST) && IsEnabled() && HasActiveTransition(sequence->m_pkOwner))
			return NiControllerSequence::ANIMATING;
		return eState;
	}

	// NiControllerSequence::GetState of a section's current sequence in Animation::Update (Animation at [ebp-0x18C], the
	// update's time step at [ebp-0x5C]), deciding whether its clock runs at the group's speed: fMoveSpeed for movement
	// groups, only for the current sequence and only while ANIMATING. With root motion moving the actor through the
	// blends of start and stop animations (RootMotionStateHook), everything blending in the movement section runs at
	// fMoveSpeed while one plays or fades, or the actor would slow to the animations' authored speed for every blend.
	// Otherwise as BlendFixes' hook
	NiControllerSequence::AnimState __fastcall ClockStateHook(NiControllerSequence* sequence)
	{
		auto* animData = GET_CALLER_VAR(AnimData*, -0x18C);
		if (IsEnabled() && sequence == animData->animSequence[kSequence_Movement] && HasActiveTransition(sequence->m_pkOwner))
		{
			const float fExtraTime = GET_CALLER_VAR(float, -0x5C) * (animData->movementSpeedMult - 1.0f);
			for (auto* other : sequence->m_pkOwner->m_kActiveSequences)
			{
				auto* bsSequence = static_cast<BSAnimGroupSequence*>(other);
				if (!bsSequence || bsSequence == sequence || NOT_TYPE(bsSequence, BSAnimGroupSequence) || !IsGait(bsSequence))
					continue;
				if (bsSequence->m_eState == NiControllerSequence::EASEOUT && !bsSequence->m_pkPartnerSequence && bsSequence->m_fOffset != -NI_INFINITY)
					bsSequence->m_fOffset += fExtraTime;
			}
			if (sequence->m_eState == NiControllerSequence::EASEIN)
				return NiControllerSequence::ANIMATING;
		}
		return BlendFixes::ContinuedVariantStateHook(sequence);
	}

	// Animation::GetGroup of the movement section of the 1st person skeleton in PlayerMover::Update: in 1st person the
	// player moves by the 1st person skeleton's movement when it plays the same group as the 3rd person one, else by the
	// 3rd person one's. A start or stop animation only plays on the skeletons that have one, and the other's movement
	// section is empty or on to another group meanwhile (a stop: blended out, its movement dropped): the player moves by
	// the 1st person skeleton while that plays one, by the 3rd person one while only that does
	UInt16 __fastcall FirstPersonMovementGroupHook(AnimData* animData, void*, eAnimSequence sequenceId)
	{
		const UInt16 groupId = animData->groupIDs[sequenceId];
		if (sequenceId != kSequence_Movement)
			return groupId;
		const auto* thirdPerson = g_thePlayer->baseProcess ? g_thePlayer->baseProcess->GetAnimData() : nullptr;
		if (!thirdPerson || thirdPerson == animData)
			return groupId;
		if (MovementTransitions::Of(animData->animSequence[kSequence_Movement]) != MovementTransition::None)
			return thirdPerson->groupIDs[kSequence_Movement];
		if (MovementTransitions::Of(thirdPerson->animSequence[kSequence_Movement]) != MovementTransition::None)
			return 0xFFFF;
		return groupId;
	}
}

void MovementTransitions::FixEndKey(BSAnimGroupSequence* sequence)
{
	if (!sequence->animGroup)
		return;
	auto& keyTimes = sequence->animGroup->keyTimes;
	if (keyTimes.Size() >= 2 && !(keyTimes[1] > keyTimes[0]))
		keyTimes[1] = sequence->m_fEndKeyTime;
}

MovementTransition MovementTransitions::GetPathTransition(std::string_view path, UInt16 groupId)
{
	if (!IsGait(groupId))
		return MovementTransition::None;
	const auto stem = sv::get_file_stem(path);
	if (sv::contains_ci(stem, "_start"))
		return MovementTransition::Start;
	if (sv::contains_ci(stem, "_stop"))
		return MovementTransition::Stop;
	return MovementTransition::None;
}

void MovementTransitions::TagPath(SavedAnims& set, AnimPath& animPath, UInt16 groupId)
{
	switch (GetPathTransition(animPath.path, groupId))
	{
	case MovementTransition::Start:
		animPath.isStartAnim = true;
		set.hasMovementTransitions = true;
		break;
	case MovementTransition::Stop:
		animPath.isStopAnim = true;
		set.hasMovementTransitions = true;
		s_hasStopAnims.store(true, std::memory_order_relaxed);
		break;
	default:
		break;
	}
}

bool MovementTransitions::HasPaths(const SavedAnims& set, MovementTransition transition)
{
	return std::ranges::any_of(set.anims, [&](const auto& path) { return GetTransition(*path) == transition; });
}

MovementTransition MovementTransitions::Of(const NiControllerSequence* sequence)
{
	if (!sequence)
		return MovementTransition::None;
	const auto* extraData = SequenceExtraDatas::Get(sequence);
	return extraData ? extraData->movementTransition : MovementTransition::None;
}

BSAnimGroupSequence* MovementTransitions::PlayMovementAnim(AnimData* animData, BSAnimGroupSequence* destAnim, UInt16 groupId)
{
	if (IsEnabled() && IsGait(groupId) && IsStanding(animData))
	{
		if (auto* set = FindOverrideSet(animData, groupId, MovementTransition::Start))
		{
			const auto paths = GetTransitionPaths(*set, MovementTransition::Start);
			if (!paths.empty())
			{
				const auto animCtx = LoadCustomAnimation(*set, *paths[GetRandomUInt(paths.size())], animData);
				auto* start = animCtx ? animCtx->anim.data : nullptr;
				if (start && Play(animData, start, groupId, MovementTransition::Start))
				{
					// at its End key Animation::Update starts the queued group, the walk or run (or blends out the section
					// when the actor no longer moves)
					animData->queuedGroupIDs[kSequence_Movement] = groupId;
					animData->queuedGroupSequenceStates[kSequence_Movement] = static_cast<UInt32>(-1);
					return start;
				}
			}
		}
	}
	// the walk or run a start or stop animation goes on to
	if (IsEnabled() && Of(animData->animSequence[kSequence_Movement]) != MovementTransition::None)
		return MovementBlendFixes::PlayMovementAnim(animData, destAnim, ToRealTime(animData, destAnim->GetEaseInTime()));
	return MovementBlendFixes::PlayMovementAnim(animData, destAnim);
}

void MovementTransitions::WriteHooks()
{
	// Animation::BlendOut of the movement section in Actor::PickAnimations (3rd person and NPCs, then the player's 1st
	// person skeleton)
	WriteRelCall(0x897735, BlendOutMovementHook);
	WriteRelCall(0x89775B, BlendOutMovementHook);
	// PlayerMover::Update
	WriteRelCall(0x9E9EB4, FirstPersonMovementGroupHook);
	// Actor::GetMovementDelta
	WriteRelCall(0x8B72F4, WarpArrivalHook);
	WriteRelCall(0x8B730A, WarpArrivalHook);
	// ActorMover::SetCurrentFrameMoveModeAndDelta
	WriteRelCall(0x9DDDD6, ArrivedShouldBeMovingHook);
}

void MovementTransitions::WriteDelayedHooks()
{
	// Actor::PickAnimations
	WriteRelCall(0x897265, PickedGroupStateHook);
	// Animation::Update
	WriteRelCall(0x492E13, RootMotionStateHook);
	// Animation::Update, after BlendFixes::WriteDelayedHooks: ClockStateHook calls BlendFixes::ContinuedVariantStateHook
	WriteRelCall(0x491B1B, ClockStateHook);
}
