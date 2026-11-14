#pragma once
#include <memory>
#include <mutex>
#include <vector>

class KFModel;

// kNVSE's uses of the game's KF models. ModelLoader::LoadKF (0x4471C0) counts a use on the KFModel it returns
// (KFModel::uiUseCount) and ModelLoader::RemoveKFModel (0x48EFF0) takes one back, freeing the model with its last:
// the engine pairs them through AnimSequenceSingle, whose destructor calls RemoveKFModel with its sequence's name.
// kNVSE never took its uses back, so every override KF an actor had played stayed loaded for the session.
// All of kNVSE's own loads and releases go through one mutex: the lookup of a model and the count on it are two steps
// in the engine, as are the check for the last use and the free.
namespace KFLoader
{
	std::mutex& Mutex();

	// Loads the KF if no one has, and counts a use. path: relative to Data\Meshes
	KFModel* Acquire(const char* path);
	void Release(const char* path);
	// With the mutex held by the caller
	KFModel* AcquireLocked(const char* path);
	void ReleaseLocked(const char* path);

	// The KF stays loaded for the session: script commands change what its sequences share (text keys, the template
	// the clones are made from), which a reload from disk would undo. Once per path (pooled)
	KFModel* Pin(const char* pooledPath);

	// KFs loaded by a worker thread ahead of their first use, so that it does not read the file on the thread that
	// animates. A batch holds a use of each KF it has loaded until it is dropped; an animation thread that needs one
	// the worker has not reached loads it itself, as before
	struct PreloadBatch;
	using Preload = std::shared_ptr<PreloadBatch>;
	// paths: pooled
	Preload StartPreload(std::vector<const char*> paths);
	void DropPreload(Preload& preload);

	// kNVSEReset: every pin and preload is released (KFs changed on disk are read again), once the worker is idle
	void ReleaseAll();
}
