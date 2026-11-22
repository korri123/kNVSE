#pragma once

#include <algorithm>
#include <span>
#include <utility>
#include <vector>

#include "NiNodes.h"

// Text keys that act at their time, parsed into items (T) sorted by time. A Context walks them for one play of a
// sequence: Update(time, f) calls f for every item the time has reached since the last update.
template <typename T>
class TimedExecution
{
public:
	class Context
	{
	public:
		float lastTime = 0;
		const TimedExecution* execution = nullptr;
		size_t index = 0;

		explicit Context(const TimedExecution<T>* execution)
			: execution(execution)
		{
		}

		Context() = default;

		bool Exists() const { return execution != nullptr; }

		// The time going back by more than 0.01 s is the sequence starting over (or looping): every item acts again
		template <typename F>
		void Update(float time, F&& f)
		{
			if (!execution || execution->items.empty())
				return;
			const auto fLastTime = this->lastTime;
			this->lastTime = time;
			if (time - fLastTime < -0.01f)
				index = 0;
			Fire(time, f);
		}

		// Starts over only once every item has acted
		template <typename F>
		void UpdateLegacy(float time, F&& f)
		{
			if (!execution || execution->items.empty())
				return;
			const auto lastTime = this->lastTime;
			this->lastTime = time;
			if (index >= execution->items.size())
			{
				if (time - lastTime < -0.01f)
					// looping anim
					index = 0;
				else
					return;
			}
			Fire(time, f);
		}

	private:
		// Calls f for the items due by time. The context is advanced past them before the first call and not touched
		// after: a callback may replace this context (a scriptLine key whose script activates its own animation again,
		// ActivateAnim -> HandleExtraOperations -> CreateContext), and going on with the replaced one fired that key
		// again at the same time, forever.
		template <typename F>
		void Fire(float time, F& f)
		{
			const auto& items = execution->items;
			const size_t first = index;
			size_t end = first;
			while (end < items.size() && time >= items[end].second)
				++end;
			index = end;
			for (size_t i = first; i < end; ++i)
				f(items[i].first);
		}
	};

	std::vector<std::pair<T, float>> items;

	TimedExecution() = default;

	// f(text, item) parses a key into an item, returning false for keys it does not take
	template <typename F>
	TimedExecution(std::span<NiTextKey> keys, F&& f)
	{
		for (auto& key : keys)
		{
			T t;
			if (f(key.m_kText.CStr(), t))
				items.emplace_back(t, key.m_fTime);
		}
		Sort();
	}

	explicit TimedExecution(std::vector<std::pair<T, float>> parsed) : items(std::move(parsed))
	{
		Sort();
	}

	// A context for a play starting at fStartTime: items before it don't act
	Context CreateContext(float fStartTime = 0.0f) const
	{
		auto context = Context(this);
		for (auto& item : items)
		{
			if (fStartTime > item.second)
				++context.index;
			else break;
		}
		context.lastTime = fStartTime;
		return context;
	}

private:
	// Hand-edited KFs can list keys out of order; the contexts walk them by time
	void Sort()
	{
		std::ranges::stable_sort(items, {}, &std::pair<T, float>::second);
	}
};
