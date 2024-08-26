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


void _Log(const std::string& msg)
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
	_MESSAGE("%s", str.c_str());
	if (g_errorLogLevel == 2)
		Console_Print("kNVSE: %s", str.c_str());
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

std::string ToLower(std::string data)
{
	ra::transform(data, data.begin(),[](const unsigned char c) { return std::tolower(c); } );
	return data;
}

std::string ToLower(std::string_view data)
{
	const std::string kData(data);
	return ToLower(kData);
}

void SetLowercase(char* data)
{
	std::transform(data, data + strlen(data), data, [](const unsigned char c) { return std::tolower(c); });
}

std::string ToLower(const char* data)
{
	return ToLower(std::string_view(data));
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

bool StartsWith(std::string_view aString, std::string_view aPrefix)
{
	if (aString.size() < aPrefix.size())
		return false;
	return StartsWith(aString.data(), aPrefix.data());
}

std::string DecompileScript(Script* script)
{
	char buffer[0x4000];
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

std::unordered_map<std::string_view, Script*, CaseInsensitiveHash, CaseInsensitiveEqual> g_conditionScripts;

Script* CompileConditionScript(std::string_view condString)
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
		scriptSource = FormatString("begin function{}\nSetFunctionValue (%s)\nend\n", condString.data());
	else
	{
		auto condStr = ReplaceAll(std::string(condString), "%r", "\r\n");
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
		ERROR_LOG("Failed to compile condition script " + std::string(condString));
		return nullptr;
	}
	auto* script = condition.release();
	iter->second = script;
	return script;
}