#include "skeleton_nodes.h"

#include <algorithm>

#include "game_types.h"
#include "GameObjects.h"
#include "GameOSDepend.h"
#include "GameProcess.h"
#include "NiObjects.h"
#include "sequence_extradata.h"
#include "utility.h"

namespace
{
    // The player's adds and target rebuilds were all on the main thread in the paths measured (equip, unequip, an
    // override's first play); not every path was
    bool CanAdd(const NiObjectNET* root)
    {
        if (!SkeletonNodes::IsPlayerSkeleton(root))
            return false;
        if (GetCurrentThreadId() == OSGlobals::GetSingleton()->mainThreadID)
            return true;
        static bool s_logged = false;
        if (!s_logged)
        {
            s_logged = true;
            ERROR_LOG("SkeletonNodes: the player's skeleton changed outside the main thread, no nodes added");
        }
        return false;
    }

    // StoreTargets looks its nodes up in the palette only (GetAVObject: the hash); the engine then gives the node a slot
    // of the multi-target controller and binds the track
    NiNode* Add(NiControllerManager* manager, NiNode* root, const SkeletonNodes::Node& node)
    {
        auto* parent = SkeletonNodes::Find(root, node.parent);
        if (!parent)
            return nullptr;
        auto* object = NiNode::CreateObject();
        object->SetName(node.name);
        // transforms set by its multi-target controller, as ResolveTransformInterpolators marks the nodes it binds
        object->m_uFlags |= NiAVObject::SELECTIVE_XFORMS_OVERRIDE_MASK;
        SkeletonNodes::Attach(parent, object);
        if (auto* palette = manager->m_spObjectPalette)
            palette->SetAVObject(node.name, object);
        return object;
    }
}

bool SkeletonNodes::IsPlayerSkeleton(const NiObjectNET* root)
{
    if (!g_thePlayer || !root)
        return false;
    const AnimData* skeletons[] = {
        g_thePlayer->baseProcess ? g_thePlayer->baseProcess->GetAnimData() : nullptr,
        g_thePlayer->firstPersonAnimData,
    };
    return std::ranges::any_of(skeletons, [&](const AnimData* animData) { return animData && animData->nSceneRoot == root; });
}

NiNode* SkeletonNodes::Require(NiControllerManager* manager, const NiFixedString& name, const NiFixedString& parent)
{
    auto* root = static_cast<NiNode*>(manager->m_pkTarget);
    if (!root)
        return nullptr;
    if (auto* object = root->GetObjectByName(name))
        return object->GetAsNiNode();
    if (!CanAdd(root))
        return nullptr;
    // only nodes the skeleton lacks are kept: a node of a model (a weapon's "##Clip") comes back with its model
    auto& nodes = kAnimationExtraData::GetOrCreate(root)->addedNodes;
    auto iter = std::ranges::find_if(nodes, [&](const Node& node) { return node.name == name; });
    if (iter == nodes.end())
        iter = nodes.insert(nodes.end(), Node{ name, parent });
    return Add(manager, root, *iter);
}

void SkeletonNodes::Restore(NiControllerManager* manager)
{
    auto* root = static_cast<NiNode*>(manager->m_pkTarget);
    const auto* extraData = root ? kAnimationExtraData::Get(root) : nullptr;
    if (!extraData || extraData->addedNodes.empty() || !CanAdd(root))
        return;
    for (const auto& node : extraData->addedNodes)
        if (!root->GetObjectByName(node.name))
            Add(manager, root, node);
}

NiNode* SkeletonNodes::Find(NiNode* root, const NiFixedString& name)
{
    auto* object = root->GetObjectByName(name);
    return object ? object->GetAsNiNode() : nullptr;
}

// NiAVObject::SetSelectiveUpdateFlags on its subtree, SELECTIVE_UPDATE and SELECTIVE_XFORMS up the parents (the selective
// update only descends into children with the first and only computes world transforms of those with the second), then
// its world transforms and bounds
void SkeletonNodes::Attach(NiNode* parent, NiAVObject* object)
{
    parent->AddObject(object, true);
    bool bSelectiveUpdate = false;
    bool bRigid = true;
    object->SetSelectiveUpdateFlags(bSelectiveUpdate, true, bRigid);
    constexpr UInt32 kFlags = NiAVObject::SELECTIVE_UPDATE_MASK | NiAVObject::SELECTIVE_XFORMS_MASK;
    for (auto* node = parent; node && (node->m_uFlags & kFlags) != kFlags; node = node->m_pkParent)
        node->m_uFlags |= kFlags;
    NiUpdateData updateData{};
    object->UpdateDownwardPass(updateData, 0);
}
