#include "GameTasks.h"
#include "Hooks_Gameplay.h"

UInt32 kModelLoader_QueueReference = 0x00444850;
UInt32 * kBSTaskCounter = (UInt32*) 0x0011C3B38;

IOManager* IOManager::GetSingleton()
{
	return *reinterpret_cast<IOManager**>(0x1202D98);
}

ModelLoader* ModelLoader::GetSingleton()
{
	return *reinterpret_cast<ModelLoader**>(0x011C3B3C);
}

void ModelLoader::QueueReference(TESObjectREFR* refr, UInt32 arg1, bool ifInMainThread)
{
	ThisStdCall(kModelLoader_QueueReference, this, refr, arg1, (UInt32)ifInMainThread);	// arg1 is encoded based on the parent cell and arg2 is most likely a boolean
}

UInt32* BSTask::GetCounterSingleton()
{
	return kBSTaskCounter;
}

