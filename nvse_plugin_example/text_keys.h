#pragma once

#include <string>
#include <vector>

#include "ammo_keys.h"
#include "burst_fire.h"
#include "timed_execution.h"

class Script;

namespace TextKeys
{
	// kNVSE's keys of a KF that HandleExtraOperations acts on, read once per KF (Find): which ones it has, and the keys that
	// act at their time in time order. Every sequence made from the KF has its text keys and its path as name.
	struct Table
	{
		enum Flag : UInt16
		{
			kRespectEndKey = 1 << 0, // respectEndKey, respectTextKeys
			kInterruptLoop = 1 << 1,
			kNoBlend = 1 << 2,
			kBlendToReloadLoop = 1 << 3,
			kAllowAttack = 1 << 4,
			kBurstFire = 1 << 5,
			kScriptCall = 1 << 6, // Script: <script editor ID>
			kScriptLine = 1 << 7, // scriptLine: <script text>
			kSoundPath = 1 << 8,  // SoundPath: <file or folder under sound\>
		};
		UInt16 flags = 0;
		// The time of the first allowAttack key
		float allowAttackTime = 0.0f;

		// The scripts of the keys that parsed; a key that did not still counts in flags
		TimedExecution<Script*> scriptCalls;
		TimedExecution<Script*> scriptLines;
		// SoundPath keys: their paths, and the keys as indices into them
		std::vector<std::string> soundPaths;
		TimedExecution<UInt32> soundKeys;
		TimedExecution<AmmoKeys::Key> ammoKeys;
		TimedExecution<BurstFire::Key> burstKeys;

		bool Has(Flag flag) const { return (flags & flag) != 0; }
		// Whether a play of the KF has keys that act after its start or need it tracked (AnimTime)
		bool NeedsTracking(bool isAttack) const;
	};

	// The KF's table, made on its first play; nullptr without text keys
	const Table* Find(const NiControllerSequence& sequence);
	// The KF's text keys were replaced (SetAnimTextKeys; every sequence of the KF shares them): its next play reads them
	// again. Plays already started keep the table they have.
	void Forget(const NiControllerSequence& sequence);
	// kNVSEReset: the KFs are read again from disk. Only while nothing holds a table (g_timeTrackedAnims is empty).
	void ClearTables();

	// Whether the text is one of kNVSE's keys, any feature's: the engine's parse of a KF's anim group skips them
	// (KeyStringCrashFixHook)
	bool IsKnvseKey(const char* text);
}
