#pragma once

#include "GameProcess.h"

class Actor;
class TESObjectWEAP;

// NPCs used to load the whole clip of a weapon with a looping reload (ReloadW-Z: Cowboy Repeater, lever-action and
// hunting shotguns, revolvers) in one round. Actor::ReloadAlt (0x8A83C0) tests IsReloadLoop and calls Actor::Reload
// (0x8A8420) on both branches, but only the player has a looping version (PlayerCharacter::ReloadLoopingWeapon
// 0x95D450). Actor::Reload fills the clip as the reload starts and queues ReloadXStart, and
// Actor::HandlePostAnimationActions (0x8BA600) continues the loop while rounds + 1 < clip. For NPCs that ended after
// the first round and then replayed the round animation as a plain reload. A reload started at once (reload type 2:
// combat AI in cover, use weapon packages) played the round animation once as a plain reload. The loop loads its
// rounds with QueueEquipObject, which leaves an NPC's clip unchanged.
// This fix makes NPCs reload like the player: the clip keeps its rounds when the reload starts and each round adds
// one, up to what Actor::Reload would have filled (the ammo the actor carries or, if its weapon uses none, the whole
// clip). The last round's animation plays to its end. The rounds it loads up to are ReloadState::loopTarget.
namespace NPCLoopingReloadFix
{
	// Actor::Reload filling the clip (the hook is AmmoKeys', which also holds fills): the rounds to fill it with. A
	// looping reload keeps the clip's rounds and records the rounds it fills it with as its target.
	SInt32 OnClipFill(Actor* actor, const TESObjectWEAP* weapon, UInt32 reloadType, bool hasClipMod, const BaseProcess::AmmoInfo& clip, SInt32 rounds);
	// Whether Actor::Reload plays a looping reload for this NPC (fix applied). One in progress counts as a full clip for
	// reload type 2 (the AI asks every update while in cover, and every call would start it over)
	bool PlaysLoop(Actor* actor, const TESObjectWEAP* weapon, bool hasClipMod);

	// modReloadAnimsFixed: whether bFixModReloadAnims is applied (the engine's looping path follows the reload anim
	// that plays)
	void ApplyHooks(bool modReloadAnimsFixed);
	void WriteDelayedHooks();
}
