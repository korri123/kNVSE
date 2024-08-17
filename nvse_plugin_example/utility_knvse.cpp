#include "commands_animation.h"
#include "GameData.h"
#include "main.h"
#include "utility.h"
#include "SafeWrite.h"

std::string GetCurPath()
{
	char buffer[MAX_PATH] = { 0 };
	GetModuleFileName(NULL, buffer, MAX_PATH);
	std::string::size_type pos = std::string(buffer).find_last_of("\\/");
	return std::string(buffer).substr(0, pos);
}

bool ends_with(std::string const& value, std::string const& ending)
{
	if (ending.size() > value.size()) return false;
	return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}

std::string ReplaceAll(std::string str, const std::string& from, const std::string& to) {
	size_t start_pos = 0;
	while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
		str.replace(start_pos, from.length(), to);
		start_pos += to.length(); // Handles case where 'to' is a substring of 'from'
	}
	return str;
}

size_t FindStringPosCI(const std::string& strHaystack, const std::string& strNeedle)
{
	const auto lowerHaystack = ToLower(std::string(strHaystack));
	const auto lowerNeedle = ToLower(std::string(strNeedle));
	return lowerHaystack.find(lowerNeedle);
}

std::string ExtractUntilStringMatches(const std::string& str, const std::string& match, bool includeMatch)
{
	const auto pos = FindStringPosCI(str, match);
	if (pos == std::string::npos)
		return "";
	return str.substr(0, pos + (includeMatch ? match.length() : 0));
}

/// Try to find in the Haystack the Needle - ignore case
bool FindStringCI(const std::string& strHaystack, const std::string& strNeedle)
{
	const auto it = ra::search(strHaystack, strNeedle,[](char ch1, char ch2) { return std::toupper(ch1) == std::toupper(ch2); }).begin();
	return it != strHaystack.end();
}

void Log(const std::string& msg)
{
	_MESSAGE("%s", msg.c_str());
	if (g_logLevel == 2)
		Console_Print("kNVSE: %s", msg.c_str());
}

int HexStringToInt(const std::string& str)
{
	char* p;
	const auto id = strtoul(str.c_str(), &p, 16);
	if (*p == 0)
		return id;
	return -1;
}

void DebugPrint(const std::string& str)
{
	if (g_logLevel == 1)
		Console_Print("kNVSE: %s", str.c_str());
	Log(str);
}

bool IsPlayersOtherAnimData(AnimData* animData)
{
	if (g_thePlayer->IsThirdPerson() && animData == g_thePlayer->firstPersonAnimData)
		return true;
	if (!g_thePlayer->IsThirdPerson() && animData == g_thePlayer->baseProcess->GetAnimData())
		return true;
	return false;
}

AnimData* GetThirdPersonAnimData(AnimData* animData)
{
	if (animData == g_thePlayer->firstPersonAnimData)
		return g_thePlayer->baseProcess->GetAnimData();
	return animData;
}

void PatchPause(UInt32 ptr)
{
	SafeWriteBuf(ptr, "\xEB\xFE", 2);
}

std::string ToLower(const std::string& data)
{
	std::string newData = data;
	ra::transform(newData, newData.begin(),[](const unsigned char c) { return std::tolower(c); } );
	return newData;
}

std::string& StripSpace(std::string&& data)
{
	std::erase_if(data, isspace);
	return data;
}

bool StartsWith(const char* string, const char* prefix)
{
	size_t count = 0;
	while (*prefix)
	{
		if (tolower(*prefix++) != tolower(*string++))
			return false;
		++count;
	}
	if (!count)
		return false;
	return true;
}

std::string DecompileScript(Script* script)
{
	char buffer[0x400];
	g_script->DecompileToBuffer(script, nullptr, buffer);
	return buffer;
}

std::filesystem::path GetRelativePath(const std::filesystem::path& fullPath, std::string_view target_dir)
{
	std::filesystem::path relativePath;
	bool found = false;
	for (const auto& part : fullPath)
	{
		if (_stricmp(part.string().c_str(), target_dir.data()) == 0)
			found = true;
		if (found)
			relativePath /= part;
	}
	return relativePath;
}

std::unordered_map<std::string, Script*> g_conditionScripts;

Script* CompileConditionScript(const std::string& condString)
{
	auto [iter, isNew] = g_conditionScripts.emplace(condString, nullptr);
	if (!isNew)
		return iter->second;
	ScriptBuffer buffer;
	const auto wasAssigningFormIDs = DataHandler::Get()->GetAssignFormIDs();
	if (wasAssigningFormIDs)
		DataHandler::Get()->SetAssignFormIDs(false);
	auto condition = MakeUnique<Script, 0x5AA0F0, 0x5AA1A0>();
	if (wasAssigningFormIDs)
		DataHandler::Get()->SetAssignFormIDs(true);
	std::string scriptSource;
	if (!FindStringCI(condString, "SetFunctionValue"))
		scriptSource = FormatString("begin function{}\nSetFunctionValue (%s)\nend\n", condString.c_str());
	else
	{
		auto condStr = ReplaceAll(condString, "%r", "\r\n");
		condStr = ReplaceAll(condStr, "%R", "\r\n");
		scriptSource = FormatString("begin function{}\n%s\nend\n", condStr.c_str());
	}
	buffer.scriptName.Set("kNVSEConditionScript");
	buffer.scriptText = scriptSource.data();
	buffer.partialScript = true;
	*buffer.scriptData = 0x1D;
	buffer.dataOffset = 4;
	buffer.currentScript = condition.get();
	const auto* ctx = ConsoleManager::GetSingleton()->scriptContext;
	const auto result = ThisStdCall<bool>(0x5AEB90, ctx, condition.get(), &buffer);
	buffer.scriptText = nullptr;
	condition->text = nullptr;
	if (!result)
	{
		DebugPrint("Failed to compile condition script " + condString);
		return nullptr;
	}
	auto* script = condition.release();
	iter->second = script;
	return script;
}