#include "text_keys.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "commands_animation.h"
#include "game_types.h"
#include "string_view_util.h"
#include "text_key_arguments.h"
#include "utility.h"

namespace TextKeys
{
	namespace
	{
		using enum Table::Flag;

		// kNVSE's key names. HandleExtraOperations' keys (flag) are matched in any case, whole or (prefix) as the start
		// of the key; the engine's group parse skips every key that starts with one of these names. Keys of other
		// features are read by their own code.
		struct KeyName
		{
			const char* name;
			UInt16 flag;
			bool prefix = false;
		};
		constexpr KeyName kKeyNames[] = {
			{ "respectEndKey", kRespectEndKey },
			{ "respectTextKeys", kRespectEndKey },
			{ "interruptLoop", kInterruptLoop },
			{ "noBlend", kNoBlend },
			{ "blendToReloadLoop", kBlendToReloadLoop },
			{ "allowAttack", kAllowAttack },
			{ "burstFire", kBurstFire },
			{ "Script:", kScriptCall, true },
			{ "scriptLine:", kScriptLine, true },
			{ "SoundPath:", kSoundPath, true },
			{ "noFix", 0 },
			{ "allowClampInLocomotion", 0 },
			{ "noBlendSmoothing", 0 },
			{ "allowFirstPerson", 0 },
		};
		// Keys with arguments (TextKeys::GetArguments)
		constexpr const char* kArgumentKeyNames[] = { "aimPitch", "weaponSpace", "spawn", "despawn", "blendCurve", "blendInCurve",
			"blendOutCurve", "loadAmmo", "unloadAmmo" };

		UInt16 GetFlag(const char* text)
		{
			for (const auto& key : kKeyNames)
			{
				if (key.flag && (key.prefix ? StartsWith(text, key.name) : !_stricmp(text, key.name)))
					return key.flag;
			}
			return 0;
		}

		Script* ParseScriptCall(const char* text)
		{
			const auto edid = GetTextAfterColon(text);
			if (edid.empty())
				return nullptr;
			auto* form = GetFormByID(edid.data());
			if (!form || !IS_ID(form, Script))
			{
				ERROR_LOG(FormatString("Text key contains invalid script %s", edid.data()));
				return nullptr;
			}
			return static_cast<Script*>(form);
		}

		// Compiled once per text for the session: the same line is often in several KFs (both views, variants)
		Script* CompileScriptLine(const char* text, const NiControllerSequence& sequence)
		{
			static std::unordered_map<std::string, Script*> s_compiled;
			const auto line = std::string(GetTextAfterColon(text));
			if (line.empty())
				return nullptr;
			auto& compiled = s_compiled[line];
			if (compiled)
				return compiled;
			auto formattedLine = line;
			FormatScriptText(formattedLine);
			compiled = Script::CompileFromText(formattedLine, "ScriptLineKey");
			if (!compiled)
				ERROR_LOG("Failed to compile script in scriptLine key: " + line + " for anim " + std::string(sequence.m_kName.CStr()));
			return compiled;
		}

		std::unique_ptr<Table> Parse(const NiControllerSequence& sequence)
		{
			auto table = std::make_unique<Table>();
			const auto keys = sequence.m_spTextKeys->GetKeys();
			for (const auto& key : keys)
			{
				const char* text = key.m_kText.CStr();
				const UInt16 flag = text ? GetFlag(text) : 0;
				if (flag == kAllowAttack && !table->Has(kAllowAttack))
					table->allowAttackTime = key.m_fTime;
				table->flags |= flag;
			}
			if (table->Has(kScriptCall))
			{
				table->scriptCalls = TimedExecution<Script*>(keys, [](const char* text, Script*& script)
				{
					script = StartsWith(text, "Script:") ? ParseScriptCall(text) : nullptr;
					return script != nullptr;
				});
			}
			if (table->Has(kScriptLine))
			{
				table->scriptLines = TimedExecution<Script*>(keys, [&](const char* text, Script*& script)
				{
					script = StartsWith(text, "scriptLine:") ? CompileScriptLine(text, sequence) : nullptr;
					return script != nullptr;
				});
			}
			if (table->Has(kSoundPath))
			{
				table->soundKeys = TimedExecution<UInt32>(keys, [&](const char* text, UInt32& index)
				{
					if (!sv::starts_with_ci(text, "SoundPath:"))
						return false;
					const auto path = GetTextAfterColon(text);
					if (path.empty())
						return false;
					index = static_cast<UInt32>(table->soundPaths.size());
					table->soundPaths.emplace_back(path);
					return true;
				});
			}
			table->ammoKeys = AmmoKeys::Parse(sequence);
			if (table->Has(kBurstFire))
				table->burstKeys = BurstFire::Parse(sequence);
			return table;
		}

		struct FixedStringHash
		{
			size_t operator()(const NiFixedString& string) const { return std::hash<const char*>()(string.CStr()); }
		};
		// By KF path. Tables are made on any thread that starts a sequence (AI threads for NPCs) and never change.
		std::mutex g_mutex;
		std::unordered_map<NiFixedString, std::unique_ptr<Table>, FixedStringHash> g_tables;
		// Tables of KFs whose keys a script replaced: plays that started before may still walk them
		std::vector<std::unique_ptr<Table>> g_replaced;
	}

	bool Table::NeedsTracking(bool isAttack) const
	{
		constexpr UInt16 kTracked = kRespectEndKey | kInterruptLoop | kNoBlend | kBlendToReloadLoop | kAllowAttack | kScriptCall
			| kScriptLine | kSoundPath;
		return (flags & kTracked) || (isAttack && Has(kBurstFire)) || !ammoKeys.items.empty();
	}

	const Table* Find(const NiControllerSequence& sequence)
	{
		if (!sequence.m_spTextKeys)
			return nullptr;
		std::unique_lock lock(g_mutex);
		auto [iter, inserted] = g_tables.try_emplace(sequence.m_kName);
		if (inserted)
			iter->second = Parse(sequence);
		return iter->second.get();
	}

	void Forget(const NiControllerSequence& sequence)
	{
		std::unique_lock lock(g_mutex);
		const auto iter = g_tables.find(sequence.m_kName);
		if (iter == g_tables.end())
			return;
		if (iter->second)
			g_replaced.push_back(std::move(iter->second));
		g_tables.erase(iter);
	}

	void ClearTables()
	{
		std::unique_lock lock(g_mutex);
		g_tables.clear();
		g_replaced.clear();
	}

	bool IsKnvseKey(const char* text)
	{
		return std::ranges::any_of(kKeyNames, [&](const KeyName& key) { return StartsWith(text, key.name); })
			|| std::ranges::any_of(kArgumentKeyNames, [&](const char* name) { return GetArguments(text, name).has_value(); });
	}
}
