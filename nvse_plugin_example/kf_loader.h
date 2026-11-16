#pragma once
#include <memory>
#include <mutex>
#include <vector>

class KFModel;

// Manages kNVSE's uses of the game's KF models. ModelLoader::LoadKF (0x4471C0) counts a use on the KFModel it returns
// (KFModel::uiUseCount), and ModelLoader::RemoveKFModel (0x48EFF0) gives one back, freeing the model on the last use.
// The engine pairs them through AnimSequenceSingle, whose destructor calls RemoveKFModel with its sequence's name.
// All of kNVSE's loads and releases go through one mutex, because the engine looks up a model and counts the use in two
// separate steps, and likewise checks for the last use and frees the model in two steps.
namespace KFLoader
{
	std::mutex& Mutex();

	// Loads the KF if it is not loaded yet and counts a use. path is relative to Data\Meshes.
	KFModel* Acquire(const char* path);
	void Release(const char* path);
	// Variants for callers that already hold the mutex.
	KFModel* AcquireLocked(const char* path);
	void ReleaseLocked(const char* path);

	// Keeps the KF loaded for the session. Script commands change what its sequences share (text keys, the template the
	// clones are made from), and a reload from disk would undo that. Takes effect once per path (pooled).
	KFModel* Pin(const char* pooledPath);

	// KFs loaded by a worker thread ahead of their first use, so the animating thread does not have to read the file.
	// A batch holds a use of each KF it has loaded until it is dropped. If an animation thread needs a KF the worker
	// has not reached yet, it loads the KF itself.
	struct PreloadBatch;
	using Preload = std::shared_ptr<PreloadBatch>;
	// paths must be pooled.
	Preload StartPreload(std::vector<const char*> paths);
	void DropPreload(Preload& preload);

	// kNVSEReset: releases every pin and preload once the worker is idle, so KFs changed on disk are read again.
	void ReleaseAll();
}
