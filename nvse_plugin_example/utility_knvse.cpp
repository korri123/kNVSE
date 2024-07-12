#include "commands_animation.h"
#include "utility.h"


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

size_t FindStringPosCI(const std::string_view strHaystack, const std::string_view strNeedle)
{
	const auto lowerHaystack = ToLower(std::string(strHaystack));
	const auto lowerNeedle = ToLower(std::string(strNeedle));
	return lowerHaystack.find(lowerNeedle);
}

std::string ExtractUntilStringMatches(const std::string_view str, const std::string_view match, bool includeMatch)
{
	const auto pos = FindStringPosCI(str, match);
	if (pos == std::string::npos)
		return "";
	return std::string(str.substr(0, pos + (includeMatch ? match.length() : 0)));
}

/// Try to find in the Haystack the Needle - ignore case
bool FindStringCI(const std::string_view strHaystack, const std::string_view strNeedle)
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

int HexStringToInt(const std::string_view str)
{
	char* p;
	const auto id = strtoul(str.data(), &p, 16);
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

bool StartsWith(std::string_view left, std::string_view right)
{
	return StartsWith(left.data(), right.data());
}
