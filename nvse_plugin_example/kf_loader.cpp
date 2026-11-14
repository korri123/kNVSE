#include "kf_loader.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_set>

#include "GameTasks.h"
#include "utility.h"

struct KFLoader::PreloadBatch
{
	std::vector<const char*> paths;
	// the worker's: what it has a use of
	std::vector<const char*> loaded;
	std::atomic<bool> dropped = false;
};

namespace
{
	struct Job
	{
		KFLoader::Preload batch;
		bool drop;
	};

	// Never destroyed: its thread runs until the process ends, past the DLL's static destructors
	struct Worker
	{
		std::mutex mutex;
		std::condition_variable wake;
		std::condition_variable idle;
		std::deque<Job> jobs;
		bool started = false;
		bool busy = false;

		void Run()
		{
			std::unique_lock lock(mutex);
			for (;;)
			{
				wake.wait(lock, [&] { return !jobs.empty(); });
				const auto job = std::move(jobs.front());
				jobs.pop_front();
				busy = true;
				lock.unlock();
				auto& batch = *job.batch;
				if (job.drop)
				{
					for (const auto* path : batch.loaded)
						KFLoader::Release(path);
					batch.loaded.clear();
				}
				else
				{
					for (const auto* path : batch.paths)
					{
						if (batch.dropped.load(std::memory_order_relaxed))
							break;
						// one file per lock: an animation thread waits for at most the file being read
						if (KFLoader::Acquire(path))
							batch.loaded.push_back(path);
					}
				}
				lock.lock();
				busy = false;
				if (jobs.empty())
					idle.notify_all();
			}
		}

		void Add(Job job)
		{
			std::scoped_lock lock(mutex);
			if (!started)
			{
				started = true;
				std::thread([this] { Run(); }).detach();
			}
			jobs.push_back(std::move(job));
			wake.notify_one();
		}

		void WaitIdle()
		{
			std::unique_lock lock(mutex);
			idle.wait(lock, [&] { return jobs.empty() && !busy; });
		}
	};

	Worker& GetWorker()
	{
		static auto* s_worker = new Worker;
		return *s_worker;
	}

	// pooled paths, with the mutex held
	std::unordered_set<const char*>& Pinned()
	{
		static auto* s_pinned = new std::unordered_set<const char*>;
		return *s_pinned;
	}
}

std::mutex& KFLoader::Mutex()
{
	static auto* s_mutex = new std::mutex;
	return *s_mutex;
}

KFModel* KFLoader::AcquireLocked(const char* path)
{
	return ModelLoader::LoadKFModel(path);
}

void KFLoader::ReleaseLocked(const char* path)
{
	// ModelLoader::RemoveKFModel
	ThisStdCall(0x48EFF0, ModelLoader::GetSingleton(), path);
}

KFModel* KFLoader::Acquire(const char* path)
{
	std::scoped_lock lock(Mutex());
	return AcquireLocked(path);
}

void KFLoader::Release(const char* path)
{
	std::scoped_lock lock(Mutex());
	ReleaseLocked(path);
}

KFModel* KFLoader::Pin(const char* pooledPath)
{
	std::scoped_lock lock(Mutex());
	auto* model = AcquireLocked(pooledPath);
	if (!model)
		return nullptr;
	// pinned before: its use stays, this one goes back
	if (!Pinned().insert(pooledPath).second)
		ReleaseLocked(pooledPath);
	return model;
}

KFLoader::Preload KFLoader::StartPreload(std::vector<const char*> paths)
{
	if (paths.empty())
		return nullptr;
	auto batch = std::make_shared<PreloadBatch>();
	batch->paths = std::move(paths);
	GetWorker().Add({ batch, false });
	return batch;
}

void KFLoader::DropPreload(Preload& preload)
{
	if (!preload)
		return;
	preload->dropped.store(true, std::memory_order_relaxed);
	// released by the worker, after anything of the batch it is still loading
	GetWorker().Add({ std::move(preload), true });
	preload.reset();
}

void KFLoader::ReleaseAll()
{
	GetWorker().WaitIdle();
	std::scoped_lock lock(Mutex());
	for (const auto* path : Pinned())
		ReleaseLocked(path);
	Pinned().clear();
}
