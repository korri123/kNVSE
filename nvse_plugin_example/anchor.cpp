#include "anchor.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "bethesda/bethesda_types.h"
#include "blend_smoothing.h"
#include "bone_evaluation.h"
#include "game_types.h"
#include "GameProcess.h"
#include "GameSettings.h"
#include "gamebryo/NiStream.h"
#include "hooks.h"
#include "sequence_extradata.h"
#include "string_view_util.h"
#include "text_key_arguments.h"
#include "transform_math.h"
#include "utility.h"

using namespace TransformMath;

struct Anchors::RestPose
{
    struct Entry
    {
        NiFixedString name; // keeps the interned string, and so the key, alive
        NiTransform local;
    };
    // By the address of the interned name: node names are NiFixedStrings, equal names share it
    std::unordered_map<const char*, Entry> nodes;

    const NiTransform* Find(const NiFixedString& name) const
    {
        const auto iter = nodes.find(name.CStr());
        return iter != nodes.end() ? &iter->second.local : nullptr;
    }

    void Add(NiAVObject* object)
    {
        if (const char* name = object->m_pcName.CStr(); name && *name)
            nodes.try_emplace(name, Entry{ object->m_pcName, CurrentLocal(object) });
        if (auto* node = object->GetAsNiNode())
            for (auto* child : node->m_children)
                if (child)
                    Add(child);
    }
};

namespace
{
    constexpr std::string_view kAnchorKey = "anchor";
    constexpr std::string_view kReferenceKey = "anchorTo";
    constexpr std::string_view kDefaultReference = "Bip01 Rotate";

    const auto kFirstPersonSkeleton = reinterpret_cast<Setting*>(0x11CDD78); // sFirstPersonSkeleton:General

    // Reads the NIF rather than taking the model the game keeps loaded: the player's first person skeleton is that
    // model itself (not a copy), so it is posed by the animations
    std::unique_ptr<Anchors::RestPose> LoadRestPose(const std::string& path)
    {
        auto pose = std::make_unique<Anchors::RestPose>();
        const std::string dataPath = "Data\\Meshes\\" + path;
        if (FileFinder::Exists(dataPath.c_str(), ARCHIVE_TYPE_MESHES))
        {
            BSStream stream;
            NiObject* object = stream.Load(dataPath.c_str()) && stream.GetObjectCount() ? stream.GetObjectAt(0) : nullptr;
            if (auto* root = NI_DYNAMIC_CAST(NiAVObject, object))
                pose->Add(root);
        }
        if (pose->nodes.empty())
            ERROR_LOG(FormatString("anchor: can't load the skeleton Meshes\\%s", path.c_str()));
        return pose;
    }

    // Kept for the session (a few skeleton files); never null
    const Anchors::RestPose* GetRestPoseOfFile(const char* modelPath)
    {
        static std::mutex s_mutex;
        static std::unordered_map<std::string, std::unique_ptr<Anchors::RestPose>> s_poses;

        std::string path = NormalizeMeshPath(modelPath ? modelPath : "");
        std::scoped_lock lock(s_mutex);
        auto& pose = s_poses[path];
        if (!pose)
            pose = LoadRestPose(path);
        return pose.get();
    }

    // The skeleton's rest pose, once Update found it
    const Anchors::RestPose* GetRestPoseOfSkeleton(const kBlendInterpolatorExtraData& extraData)
    {
        auto* root = extraData.owner ? extraData.owner->m_pkTarget : nullptr;
        const auto* animationData = root ? kAnimationExtraData::Get(root) : nullptr;
        const auto* pose = animationData ? animationData->restPose : nullptr;
        return pose && !pose->nodes.empty() ? pose : nullptr;
    }

    // The reference of the first attached anchored item: Apply anchors only the items with that one
    NiAVObject* GetActiveReference(const kBlendInterpolatorExtraData& extraData)
    {
        for (const auto& item : extraData.items)
            if (item.anchored && item.interpolator)
                return item.anchorReference;
        return nullptr;
    }

    // The bones between the reference and a bone it is above, from the bone's parent up
    class Chain
    {
    public:
        // False if reference is no longer above bone
        bool Build(const NiAVObject& bone, const NiAVObject* reference)
        {
            for (NiAVObject* node = bone.m_pkParent; node; node = node->m_pkParent)
            {
                if (node == reference)
                    return true;
                if (m_count == kMaxDepth)
                    return false;
                m_bones[m_count++] = node;
            }
            return false;
        }

        // B: the bones' locals composed from the top down, each evaluated by this pass first
        NiTransform ComposeCurrent() const
        {
            NiTransform result = IdentityTransform();
            for (unsigned int i = m_count; i-- > 0;)
            {
                BoneEvaluation::Require(m_bones[i]);
                result = result * CurrentLocal(m_bones[i]);
            }
            return result;
        }

        // B0: the same at rest; null if a bone is not in the NIF
        const NiAVObject* ComposeRest(const Anchors::RestPose& pose, NiTransform& result) const
        {
            result = IdentityTransform();
            for (unsigned int i = m_count; i-- > 0;)
            {
                const NiTransform* local = pose.Find(m_bones[i]->m_pcName);
                if (!local)
                    return m_bones[i];
                result = result * *local;
            }
            return nullptr;
        }

    private:
        static constexpr unsigned int kMaxDepth = 32;
        NiAVObject* m_bones[kMaxDepth];
        unsigned int m_count = 0;
    };

    // Recomputes the rest chain when the reference, the bone's parent or the skeleton changed
    void UpdateRest(Anchors::NodeData& data, const NiAVObject& bone, const NiAVObject* reference, const Chain& chain,
        const Anchors::RestPose& pose)
    {
        if (data.reference == reference && data.parent == bone.m_pkParent && data.restPose == &pose)
            return;
        data.reference = reference;
        data.parent = bone.m_pkParent;
        data.restPose = &pose;
        if (const auto* missing = chain.ComposeRest(pose, data.restChain))
        {
            ERROR_LOG_ONCE(FormatString("anchor: %s is not in the skeleton's NIF, %s is not anchored", missing->m_pcName.CStr(),
                bone.m_pcName.CStr()));
            data.restValid = false;
            return;
        }
        const NiTransform* local = pose.Find(bone.m_pcName);
        data.restLocal = local ? *local : CurrentLocal(&bone);
        data.restValid = true;
    }
}

Anchors::Keys::Keys(const NiControllerSequence& sequence) : m_sequence(sequence)
{
    const auto bones = TextKeys::FindArguments(sequence, kAnchorKey);
    if (!bones)
        return;
    TextKeys::ArgumentReader reader(*bones);
    while (const auto bone = reader.Next())
    {
        if (bone->empty())
            continue;
        if (m_count == kMaxBones)
        {
            ERROR_LOG(FormatString("anchor: more than %u bones in %s", kMaxBones, sequence.m_kName.CStr()));
            break;
        }
        m_bones[m_count++] = *bone;
    }
    const auto reference = TextKeys::FindArguments(sequence, kReferenceKey);
    m_reference = reference && !reference->empty() ? *reference : kDefaultReference;
}

NiAVObject* Anchors::Keys::GetReference(const NiAVObject& target) const
{
    const char* name = target.m_pcName.CStr();
    if (!name || std::none_of(m_bones, m_bones + m_count, [&](std::string_view bone) { return sv::equals_ci(bone, name); }))
        return nullptr;
    for (NiAVObject* ancestor = target.m_pkParent; ancestor; ancestor = ancestor->m_pkParent)
        if (const char* ancestorName = ancestor->m_pcName.CStr(); ancestorName && sv::equals_ci(m_reference, ancestorName))
            return ancestor;
    ERROR_LOG_ONCE(FormatString("anchor: %.*s is not above %s in the skeleton, %s does not anchor it (anchorTo names another bone)",
        static_cast<int>(m_reference.size()), m_reference.data(), name, m_sequence.m_kName.CStr()));
    return nullptr;
}

void Anchors::Attach(kBlendInterpolatorExtraData& extraData, kBlendInterpItem& extraItem, NiAVObject* reference)
{
    extraItem.anchored = reference != nullptr;
    extraItem.anchorReference = reference;
    if (!reference)
        return;
    if (!extraData.anchor)
        extraData.anchor = std::make_unique<NodeData>();
    if (auto* root = extraData.owner ? extraData.owner->m_pkTarget : nullptr)
        if (auto* animationData = kAnimationExtraData::GetOrCreate(root))
            animationData->wantsRestPose = true;
}

void Anchors::Update(AnimData* animData, Actor* actor)
{
    if (!g_pluginSettings.blendSmoothing || !animData || !actor)
        return;
    auto* root = animData->controllerManager ? animData->controllerManager->m_pkTarget : nullptr;
    auto* animationData = root ? kAnimationExtraData::Get(root) : nullptr;
    if (!animationData || !animationData->wantsRestPose || animationData->restPose)
        return;
    // The skeleton files the game loads: PlayerCharacter::Load3D, TESObjectREFR::InitAnimation
    const bool firstPerson = animData == PlayerCharacter::GetSingleton()->firstPersonAnimData;
    animationData->restPose = GetRestPoseOfFile(firstPerson ? kFirstPersonSkeleton->data.str : actor->GetModel());
}

void Anchors::Prepare(kBlendInterpolatorExtraData* extraData, NiObjectNET* target)
{
    if (!extraData || !extraData->anchor)
        return;
    NodeData& data = *extraData->anchor;
    data.correctionValid = false;

    const auto& bone = *static_cast<NiAVObject*>(target);
    const NiAVObject* reference = GetActiveReference(*extraData);
    const RestPose* pose = reference ? GetRestPoseOfSkeleton(*extraData) : nullptr;
    Chain chain;
    if (!pose || !chain.Build(bone, reference))
        return;
    UpdateRest(data, bone, reference, chain, *pose);
    if (!data.restValid)
        return;
    data.correction = Inverse(chain.ComposeCurrent()) * data.restChain;
    data.correctionValid = true;
}

void Anchors::Apply(const kBlendInterpolatorExtraData* extraData, const kBlendInterpItem* extraItem, NiQuatTransform& kTransform)
{
    if (!extraItem || !extraItem->anchored)
        return;
    const NodeData* data = extraData->anchor.get();
    if (!data || !data->correctionValid || data->reference != extraItem->anchorReference)
        return;
    // The item's channels over the bone's rest local
    NiTransform local = data->restLocal;
    if (kTransform.IsTranslateValid())
        local.m_Translate = kTransform.GetTranslate();
    if (kTransform.IsRotateValid())
        ToUnitRotation(kTransform.GetRotate(), local.m_Rotate);
    if (kTransform.IsScaleValid())
        local.m_fScale = kTransform.GetScale();
    const NiTransform anchored = data->correction * local;
    kTransform.SetTranslate(anchored.m_Translate);
    kTransform.SetRotate(anchored.m_Rotate);
    if (kTransform.IsScaleValid())
        kTransform.SetScale(anchored.m_fScale);
}
