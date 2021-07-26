#pragma once
#include <algorithm>
#include <string>
#include "GameAPI.h"
#include "SafeWrite.h"
#include "GameProcess.h"
#include "GameObjects.h"
extern int g_logLevel;

std::string GetCurPath();

bool ends_with(std::string const& value, std::string const& ending);

std::string ReplaceAll(std::string str, const std::string& from, const std::string& to);

/// Try to find in the Haystack the Needle - ignore case
bool FindStringCI(const std::string& strHaystack, const std::string& strNeedle);

void Log(const std::string& msg);

int HexStringToInt(const std::string& str);

void DebugPrint(const std::string& str);

// if player is in third person, returns true if anim data is the first person and vice versa
bool IsPlayersOtherAnimData(AnimData* animData);

AnimData* GetThirdPersonAnimData(AnimData* animData);

void PatchPause(UInt32 ptr);

template <typename T>
bool In(const T& t, std::initializer_list<T> l)
{
	for (auto i : l)
		if (l == t)
			return true;
	return false;
}
