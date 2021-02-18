#pragma once
#include <algorithm>
#include <string>

inline std::string GetCurPath()
{
	char path[MAX_PATH];
	GetCurrentDirectory(MAX_PATH, path);
	return path;
}

inline std::string GetScriptsDir()
{
	return GetCurPath() + "\\Data\\Scripts";
}


inline bool ends_with(std::string const& value, std::string const& ending)
{
	if (ending.size() > value.size()) return false;
	return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}

inline std::string ReplaceAll(std::string str, const std::string& from, const std::string& to) {
	size_t start_pos = 0;
	while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
		str.replace(start_pos, from.length(), to);
		start_pos += to.length(); // Handles case where 'to' is a substring of 'from'
	}
	return str;
}

/// Try to find in the Haystack the Needle - ignore case
inline bool FindStringCI(const std::string& strHaystack, const std::string& strNeedle)
{
	const auto it = std::search(
		strHaystack.begin(), strHaystack.end(),
		strNeedle.begin(), strNeedle.end(),
		[](char ch1, char ch2) { return std::toupper(ch1) == std::toupper(ch2); }
	);
	return it != strHaystack.end();
}

inline void Log(const std::string& msg)
{
	_MESSAGE(msg.c_str());
}

inline int HexStringToInt(const std::string& str)
{
	char* p;
	const auto id = strtoul(str.c_str(), &p, 16);
	if (*p == 0)
		return id;
	return -1;
}

