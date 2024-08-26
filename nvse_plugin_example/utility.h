#pragma once
#include <algorithm>
#include <functional>
#include <PluginAPI.h>
#include <string>
#include "GameAPI.h"
#include "GameProcess.h"
#include "GameObjects.h"
#include <memory>
#include <iterator>
#include <filesystem>

extern int g_logLevel;
extern int g_errorLogLevel;
namespace ra = std::ranges;


template<typename ... Bases>
struct overload : Bases ...
{
	using is_transparent = void;
	using Bases::operator() ... ;
};


struct char_pointer_hash
{
	auto operator()( const char* ptr ) const noexcept
	{
		return std::hash<std::string_view>{}( ptr );
	}
};

using transparent_string_hash = overload<
	std::hash<std::string>,
	std::hash<std::string_view>,
	char_pointer_hash
>;

std::string GetCurPath();

bool ends_with(std::string const& value, std::string const& ending);

std::string ReplaceAll(std::string str, const std::string& from, const std::string& to);

inline bool FindStringCI(const std::string_view strHaystack, const std::string_view strNeedle)
{
	const auto it = ra::search(strHaystack, strNeedle, [](char ch1, char ch2) { return std::tolower(ch1) == std::tolower(ch2); }).begin();
	return it != std::ranges::end(strHaystack);
}

inline bool FindStringCI(const char* strHaystack, const char* strNeedle)
{
	return FindStringCI(std::string_view(strHaystack), std::string_view(strNeedle));
}

inline size_t FindStringPosCI(const std::string_view strHaystack, const std::string_view strNeedle)
{
    const auto it = ra::search(strHaystack,strNeedle,
        [](char ch1, char ch2) { return std::tolower(ch1) == std::tolower(ch2); }
    );
	return it.begin() == strHaystack.end() ? std::string::npos : std::distance(strHaystack.begin(), it.begin());
}

inline std::string_view ExtractUntilStringMatches(const std::string_view str, const std::string_view match, bool includeMatch)
{
	const auto pos = FindStringPosCI(str, match);
	if (pos == std::string::npos)
		return "";
	return str.substr(0, pos + (includeMatch ? match.length() : 0));
}

void _Log(const std::string& msg);
#define LOG(msg) do { if (g_logLevel >= 1) _Log(msg); } while (0)


int HexStringToInt(const std::string& str);

void DebugPrint(const std::string& str);
#define ERROR_LOG(msg) do { if (g_errorLogLevel >= 1) DebugPrint(msg); } while (0)

// if player is in third person, returns true if anim data is the first person and vice versa
bool IsPlayersOtherAnimData(AnimData* animData);

AnimData* GetThirdPersonAnimData(AnimData* animData);

void PatchPause(UInt32 ptr);

template <typename T>
bool In(T t, std::initializer_list<T> l)
{
	for (auto i : l)
		if (l == t)
			return true;
	return false;
}

#define _L(x, y) [&](x) {return y;}
#define _VL(x, y)[&]x {return y;} // variable parenthesis lambda

std::string ToLower(std::string data);
std::string ToLower(std::string_view data);
std::string ToLower(const char* data);

bool StartsWith(const char* string, const char* prefix);
bool StartsWith(std::string_view left, std::string_view right);

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

template <typename T, const UInt32 ConstructorPtr = 0, typename... Args>
T* New(Args &&... args)
{
	auto* alloc = FormHeap_Allocate(sizeof(T));
	if constexpr (ConstructorPtr)
	{
		ThisStdCall(ConstructorPtr, alloc, std::forward<Args>(args)...);
	}
	else
	{
		memset(alloc, 0, sizeof(T));
	}
	return static_cast<T*>(alloc);
}

template <typename T, const UInt32 DestructorPtr = 0, typename... Args>
void Delete(T* t, Args &&... args)
{
	if constexpr (DestructorPtr)
	{
		ThisStdCall(DestructorPtr, t, std::forward<Args>(args)...);
	}
	FormHeap_Free(t);
}

template <typename T>
using game_unique_ptr = std::unique_ptr<T, std::function<void(T*)>>;

template <typename T, const UInt32 DestructorPtr = 0>
game_unique_ptr<T> MakeUnique(T* t)
{
	return game_unique_ptr<T>(t, [](T* t2) { Delete<T, DestructorPtr>(t2); });
}

template <typename T, const UInt32 ConstructorPtr = 0, const UInt32 DestructorPtr = 0, typename... ConstructorArgs>
game_unique_ptr<T> MakeUnique(ConstructorArgs &&... args)
{
	auto* obj = New<T, ConstructorPtr>(std::forward<ConstructorArgs>(args)...);
	return MakeUnique<T, DestructorPtr>(obj);
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

#define INLINE_HOOK(retnType, callingConv, ...) static_cast<retnType(callingConv*)(__VA_ARGS__)>([](__VA_ARGS__) [[msvc::forceinline]] -> retnType

class NVSEStringMapBuilder
{
public:
	void Add(const std::string& key, const NVSEArrayVarInterface::Element& value) {
		keys.emplace_back(key);
		values.emplace_back(value);
	}

	NVSEArrayVarInterface::Array* Build(NVSEArrayVarInterface* arrayVarInterface, Script* callingScript) {
		std::vector<const char*> cstr_keys;
		ra::transform(keys, std::back_inserter(cstr_keys),
		                       [](const std::string& str) { return str.c_str(); });

		return arrayVarInterface->CreateStringMap(cstr_keys.data(), values.data(), keys.size(), callingScript);
	}

private:
	std::vector<std::string> keys;
	std::vector<NVSEArrayVarInterface::Element> values;
};

class NVSEMapBuilder
{
public:
	void Add(double key, const NVSEArrayVarInterface::Element& value) {
		keys.emplace_back(key);
		values.emplace_back(value);
	}

	NVSEArrayVarInterface::Array* Build(NVSEArrayVarInterface* arrayVarInterface, Script* callingScript) {
		return arrayVarInterface->CreateMap(keys.data(), values.data(), keys.size(), callingScript);
	}

private:
	std::vector<double> keys;
	std::vector<NVSEArrayVarInterface::Element> values;
};

class NVSEArrayBuilder
{
public:
	void Add(const NVSEArrayVarInterface::Element& value)
	{
		values.emplace_back(value);
	}
	NVSEArrayVarInterface::Array* Build(NVSEArrayVarInterface* arrayVarInterface, Script* callingScript)
	{
		return arrayVarInterface->CreateArray(values.data(), values.size(), callingScript);
	}
private:
	std::vector<NVSEArrayVarInterface::Element> values;
};

class NVSECommandBuilder
{
	const NVSEInterface* scriptInterface;
public:
	explicit NVSECommandBuilder(const NVSEInterface* scriptInterface) : scriptInterface(scriptInterface) {}

	void Create(const char* name, CommandReturnType returnType, std::initializer_list<ParamInfo> params, bool refRequired, Cmd_Execute fn, Cmd_Parse parse = nullptr, const char* altName = "") const
	{
		ParamInfo* paramCopy = nullptr;
		if (params.size())
		{
			paramCopy = new ParamInfo[params.size()];
			int index = 0;
			for (const auto& param : params)
			{
				paramCopy[index++] = param;
			}
		}
		
		auto commandInfo = CommandInfo{
			name, altName, 0, "", refRequired, static_cast<UInt16>(params.size()), paramCopy, fn, parse, nullptr, 0
		};
		scriptInterface->RegisterTypedCommand(&commandInfo, returnType);
	}

};

template <typename K, typename V>
std::map<K, V> NVSEMapToMap(const NVSEArrayVarInterface* arrayVarInterface, NVSEArrayVarInterface::Array* arr)
{
	std::map<K, V> map;
	const auto size = arrayVarInterface->GetArraySize(arr);
	std::vector<NVSEArrayVarInterface::Element> keys(size);
	std::vector<NVSEArrayVarInterface::Element> values(size);
	if (!arrayVarInterface->GetElements(arr, values.data(), keys.data()))
		return map;
	for (int i = 0; i < size; ++i)
	{
		auto keyElem = keys.at(i);
		auto valueElem = values.at(i);
		map.emplace(keyElem.Get<K>(), valueElem.Get<V>());
	}
	return map;
}

template <typename T>
std::vector<T> NVSEArrayToVector(const NVSEArrayVarInterface* arrayVarInterface, NVSEArrayVarInterface::Array* arr)
{
	std::vector<T> vec;
	const auto size = arrayVarInterface->GetArraySize(arr);
	if (size <= 0)
		return vec;
	vec.reserve(size);
	std::vector<NVSEArrayVarInterface::Element> keys(size);
	std::vector<NVSEArrayVarInterface::Element> elements(size);
	if (!arrayVarInterface->GetElements(arr, elements.data(), keys.data()))
		return vec;
	for (int i = 0; i < size; ++i)
	{
		auto elem = elements.at(i);
		vec.emplace_back(elem.Get<T>());
	}
	return vec;
}

inline UInt8* GetParentBasePtr(void* addressOfReturnAddress, bool lambda = false)
{
	auto* basePtr = static_cast<UInt8*>(addressOfReturnAddress) - 4;
#if _DEBUG
	if (lambda) // in debug mode, lambdas are wrapped inside a closure wrapper function, so one more step needed
		basePtr = *reinterpret_cast<UInt8**>(basePtr);
#endif
	return *reinterpret_cast<UInt8**>(basePtr);
}

inline UInt32* GetLambdaAddrOfRetnAddr(void* addressOfReturnAddress)
{
#if _DEBUG
	auto* basePtr = static_cast<UInt8*>(addressOfReturnAddress) - 4;
	basePtr = *reinterpret_cast<UInt8**>(basePtr);
	return reinterpret_cast<UInt32*>(basePtr + 4);
#else
	return static_cast<UInt32*>(addressOfReturnAddress);

#endif
}

template <typename T>
T GetVariableOfCallerStack(void* addressOfReturnAddress, int offset, bool lambda = false)
{
	auto* basePtr = GetParentBasePtr(addressOfReturnAddress, lambda);
	return *reinterpret_cast<T*>(basePtr + offset);
}

template <typename T>
T GetVariablePtrOfCallerStack(void* addressOfReturnAddress, int offset, bool lambda = false)
{
	auto* basePtr = GetParentBasePtr(addressOfReturnAddress, lambda);
	return reinterpret_cast<T>(basePtr + offset);
}

template <typename T>
T GetOptimizedVariableOfCallerStack(void* addressOfReturnAddress, int offset, bool lambda = false)
{
	return *reinterpret_cast<T*>(static_cast<UInt8*>(addressOfReturnAddress) + offset);
}

#define GET_CALLER_VAR(type, offset) GetVariableOfCallerStack<type>(_AddressOfReturnAddress(), offset, false)
#define GET_CALLER_VAR_LAMBDA(type, offset) GetVariableOfCallerStack<type>(_AddressOfReturnAddress(), offset, true)
#define GET_CALLER_VAR_PTR(type, offset) GetVariablePtrOfCallerStack<type>(_AddressOfReturnAddress(), offset, false)
#define GET_CALLER_VAR_PTR_LAMBDA(type, offset) GetVariablePtrOfCallerStack<type>(_AddressOfReturnAddress(), offset, true)

// Hash function for std::pair
struct pair_hash {
	template <class T1, class T2>
	std::size_t operator()(const std::pair<T1, T2>& p) const {
		const auto hash1 = std::hash<T1>()(p.first);
		const auto hash2 = std::hash<T2>()(p.second);
		return hash_combine(hash1, hash2);
	}

private:
	// A better way to combine hashes
	static std::size_t hash_combine(std::size_t seed, std::size_t hash) {
		return seed ^ (hash + 0x9e3779b9 + (seed << 6) + (seed >> 2));
	}
};

// Custom equality comparator
struct pair_equal {
	template <class T1, class T2>
	bool operator()(const std::pair<T1, T2>& lhs, const std::pair<T1, T2>& rhs) const {
		return lhs.first == rhs.first && lhs.second == rhs.second;
	}
};

std::string DecompileScript(Script* script);

std::filesystem::path GetRelativePath(const std::filesystem::path& fullPath, std::string_view target_dir);

Script* CompileConditionScript(std::string_view condString);

struct CaseInsensitiveHash {
	std::size_t operator()(std::string_view str) const {
		std::size_t hash = 0;
		std::hash<char> charHash;
		for (char c : str) {
			hash ^= charHash(std::tolower(c)) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
		}
		return hash;
	}
};

// Custom equality function that ignores case
struct CaseInsensitiveEqual
{
	bool operator()(std::string_view lhs, std::string_view rhs) const
	{
		return ra::equal(lhs, rhs,
		                 [](char a, char b)
		                 {
			                 return std::tolower(a) == std::tolower(b);
		                 });
	}
};

void SetLowercase(char* data);