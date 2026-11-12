#pragma once

#include "NiNodes.h"

// Nodes kNVSE adds to an animated skeleton (the target of a NiControllerManager) for KFs to animate like any bone. A KF
// only finds nodes that are in the manager's object palette when it is bound (NiControllerSequence::StoreTargets), and
// the engine rebuilds the palette from the scene graph in Animation::ReloadTargets, where a node whose parent went with
// a model (the weapon's) is gone. So the nodes a skeleton was given are kept with it, in kAnimationExtraData::addedNodes
// on the manager's target, and added again there while their parent is back (Restore, from the ReloadTargets hook).
// To have a KF's node bound with its other tracks, Require it when the sequence joins the manager
// (SequenceExtraDatas' hook in NiControllerManager::AddSequence).
namespace SkeletonNodes
{
    struct Node
    {
        NiFixedString name;
        NiFixedString parent;
    };

    // Whether root is the scene root of one of the player's skeletons (third person, first person)
    bool IsPlayerSkeleton(const NiObjectNET* root);

    // The node of that name in the manager's skeleton; null if it has none and none could be added. A skeleton without
    // it is given one under the node named parent, now or, while it has no such parent, when it next reloads its
    // targets. The player's skeletons on the main thread only: other actors bind their KFs on the AI threads, while
    // the main thread renders and culls their 3D, and adding a node there would race with it.
    NiNode* Require(NiControllerManager* manager, const NiFixedString& name, const NiFixedString& parent);

    // Animation::ReloadTargets refilled the manager's palette from the scene graph: adds the skeleton's nodes that are
    // gone and whose parent is there, before the engine binds every sequence again
    void Restore(NiControllerManager* manager);

    NiNode* Find(NiNode* root, const NiFixedString& name);

    // Adds the object under a node of a skeleton and makes the skeleton's selective update reach it
    void Attach(NiNode* parent, NiAVObject* object);
}
