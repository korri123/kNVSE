#pragma once
#include <functional>
#include <deque>
#include <PluginAPI.h>
#include <shared_mutex>
#include <thread>

#include "commands_animation.h"

struct CustomAnimGroupScript
{
	LambdaVariableContext script;
	LambdaVariableContext cleanUpScript;

	friend bool operator==(const CustomAnimGroupScript& lhs, const CustomAnimGroupScript& rhs)
	{
		return lhs.script == rhs.script
			&& lhs.cleanUpScript == rhs.cleanUpScript;
	}

	friend bool operator!=(const CustomAnimGroupScript& lhs, const CustomAnimGroupScript& rhs)
	{
		return !(lhs == rhs);
	}
};

extern NVSEScriptInterface* g_script;
extern NVSECommandTableInterface* g_cmdTable;
extern NVSEDataInterface* g_nvseDataInterface;

extern std::deque<std::function<void()>> g_synchronizedExecutionQueue;
extern ICriticalSection g_executionQueueCS;
extern NVSEArrayVarInterface* g_arrayVarInterface;
extern NVSEStringVarInterface* g_stringVarInterface;
extern std::unordered_map<std::string, std::vector<CustomAnimGroupScript>> g_customAnimGroups;
extern std::vector<std::string> g_eachFrameScriptLines;

bool IsGodMode();
bool IsDemigodMode();

float GetAnimTime(const NiControllerSequence* anim);
void HandleCustomTextKeys();

// Calls a condition script for the reference. On the threads that pick animations every frame (the main thread, AI
// linear task thread 1) the result is kept until the frame's caches are cleared: the same condition is asked for
// every group looked up
bool CallFunction(Script* funcScript, TESObjectREFR* callingObj, TESObjectREFR* container,
		NVSEArrayVarInterface::Element* result);

// dead, deleted, or without a process or 3D
bool IsActorInvalid(Actor* actor);

struct MapHitCounter
{
	const char* name;
	int hits = 0;
	int misses = 0;
	int total = 0;

	void Print()
	{
		Console_Print("%s Hits: %d misses: %d total: %d", name, hits, misses, total);
		hits = 0;
		misses = 0;
		total = 0;
	}
};

struct MapHitCounters
{
	MapHitCounter getActorAnimation{"GetActorAnimation"};
	MapHitCounter scriptCall{"ScriptCall"};
};

extern MapHitCounters g_mapHitCounters;

struct AverageTimer
{
	const char* name;
	unsigned int count = 0;
	std::chrono::high_resolution_clock::duration totalDuration = std::chrono::high_resolution_clock::duration::zero();

	void Add(std::chrono::high_resolution_clock::duration duration)
	{
		count++;
		totalDuration += duration;
	}

	void Print()
	{
		if (count == 0)
			return;
		long long average = std::chrono::duration_cast<std::chrono::nanoseconds>(totalDuration).count() / count;
		char buf[0x100];
		sprintf_s(buf, 0x100, "%lld ns", average);
		Console_Print("%s", buf);
		if (count % 1000 == 0)
		{
			count = 0;
			totalDuration = std::chrono::high_resolution_clock::duration::zero();
		}
	}
};

struct FunctionTimer
{
	AverageTimer* timer;
	std::chrono::high_resolution_clock::time_point startTime;
	std::chrono::high_resolution_clock::time_point endTime;

	FunctionTimer(AverageTimer* timer) : timer(timer)
	{
		startTime = std::chrono::high_resolution_clock::now();
	}

	~FunctionTimer()
	{
		endTime = std::chrono::high_resolution_clock::now();
		timer->Add(endTime - startTime);
	}

};

struct AverageTimers
{
	AverageTimer getActorAnimation{"GetActorAnimation"};
};

extern AverageTimers g_averageTimers;

void ClearResultCaches();

class SynchronizedQueue
{
	std::deque<std::function<void()>> queue;
	std::mutex mutex;
public:
	void Add(std::function<void()>&& func);
	void RunAllAndClear();
};
extern SynchronizedQueue g_mainThreadQueue;
