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

#define _L(x, y) [&](x) {return y;}
#define _VL(x, y)[&]x {return y;} // variable parenthesis lambda

namespace ra = std::ranges;

std::string& ToLower(std::string&& data);
std::string& StripSpace(std::string&& data);

bool StartsWith(const char* left, const char* right);

template <typename T, typename S, typename F>
std::vector<T*> Filter(const S& s, const F& f)
{
	std::vector<T*> vec;
	for (auto& i : s)
	{
		if (f(i))
			vec->push_back(&i);
	}
	return vec;
}

template <typename T, typename V, typename F>
std::vector<T> MapTo(const V& v, const F& f)
{
	std::vector<T> vec;
	for (auto& i : v)
	{
		vec.emplace_back(f(i));
	}
	return vec;
}

#define SIMPLE_HOOK(address, name, retnOffset) \
static constexpr auto kAddr_#name = address; \
__declspec(naked) void Hook_#name() \
{ \
	const static auto retnAddr = (address) + (retnOffset); \
	__asm\
	{\
		mov ecx, ebp \
		call Handle#name \
		jmp retnAddress \
	} \
}

#define JMP_HOOK(address, name, retnAddress, ...) \
static constexpr auto kAddr_##name = address; \
__declspec(naked) void Hook_##name() \
{ \
	const static auto retnAddr = retnAddress; \
	__asm \
		__VA_ARGS__ \
} \

#define APPLY_JMP(name) WriteRelJump(kAddr_##name, Hook_##name)

#define _A __asm
