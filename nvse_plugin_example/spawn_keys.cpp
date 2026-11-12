#include "spawn_keys.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "game_types.h"
#include "GameObjects.h"
#include "GameProcess.h"
#include "GameTasks.h"
#include "NiObjects.h"
#include "sequence_extradata.h"
#include "skeleton_nodes.h"
#include "string_view_util.h"
#include "utility.h"

namespace
{
    constexpr std::string_view kSpawnKey = "spawn:";
    constexpr std::string_view kDespawnKey = "despawn:";
    constexpr const char* kDefaultParent = "Weapon";

    // A mesh shown by an animation
    struct Spawned
    {
        NiPointer<NiControllerSequence> sequence;
        NiFixedString bone;
        std::string mesh;
        // held so detaching stays safe when the skeleton goes first (~NiNode clears its children's parent)
        NiPointer<NiNode> boneNode;
        NiPointer<NiAVObject> object;
        UInt32 frame;
    };

    std::vector<Spawned> g_spawned;
    // EndFrame removes what no Update asked for since the previous EndFrame
    UInt32 g_frame = 0;
    // the loaded models, cloned for every spawn; null when the file failed to load. Lowercase paths under Data\Meshes
    std::unordered_map<std::string, NiPointer<NiNode>> g_models;

    void LogOnce(const std::string& message)
    {
        static std::unordered_set<std::string> s_logged;
        if (s_logged.insert(message).second)
            ERROR_LOG("SpawnKeys: " + message);
    }

    std::string_view Trim(std::string_view text)
    {
        const auto first = text.find_first_not_of(" \t");
        if (first == std::string_view::npos)
            return {};
        return text.substr(first, text.find_last_not_of(" \t") - first + 1);
    }

    std::vector<std::string_view> SplitArguments(std::string_view text)
    {
        std::vector<std::string_view> result;
        while (true)
        {
            const auto comma = text.find(',');
            result.push_back(Trim(text.substr(0, comma)));
            if (comma == std::string_view::npos)
                return result;
            text.remove_prefix(comma + 1);
        }
    }

    bool IsNifPath(std::string_view mesh)
    {
        return sv::ends_with_ci(mesh, ".nif");
    }

    void RemoveCollision(NiAVObject* object)
    {
        if (auto* collision = static_cast<NiRefObject*>(object->m_spCollisionObject))
        {
            object->m_spCollisionObject = nullptr;
            collision->DecrementRefCount();
        }
        if (auto* node = object->GetAsNiNode())
            for (auto* child : node->m_children)
                if (child)
                    RemoveCollision(child);
    }

    void PrefixNames(NiAVObject* object, std::string_view prefix)
    {
        if (const char* name = object->m_pcName.CStr(); name && *name)
            object->SetName(NiFixedString((std::string(prefix) + name).c_str()));
        if (auto* node = object->GetAsNiNode())
            for (auto* child : node->m_children)
                if (child)
                    PrefixNames(child, prefix);
    }

    NiNode* LoadModel(std::string_view mesh)
    {
        std::string path(mesh);
        std::ranges::replace(path, '/', '\\');
        for (const std::string_view prefix : { "data\\", "meshes\\" })
            if (sv::starts_with_ci(path, prefix))
                path.erase(0, prefix.size());
        auto [iter, inserted] = g_models.try_emplace(ToLower(path), nullptr);
        if (inserted)
        {
            // FileFinder::Locate, as BipedAnim::Resolve1stFileName looks for meshes (loose files and archives): LoadFile
            // would return the error marker for a missing one
            const std::string dataPath = "Data\\Meshes\\" + path;
            if (!CdeclCall<BOOL>(0x456A20, dataPath.c_str(), static_cast<char*>(nullptr), 6, 1))
                LogOnce(FormatString("no file Meshes\\%s", path.c_str()));
            else
            {
                // ModelLoader::LoadFile: the cached model or a new one, loaded now, shaders assigned
                iter->second = ThisStdCall<NiNode*>(0x447080, ModelLoader::GetSingleton(), path.c_str(), 0, true, 0, false, false);
                if (!iter->second)
                    LogOnce(FormatString("can't load Meshes\\%s", path.c_str()));
            }
        }
        return iter->second;
    }

    // A copy of the mesh, ready to attach under the bone
    NiPointer<NiAVObject> CreateObject(AnimData* animData, const NiControllerSequence& sequence, const SpawnKeys::Bone& bone,
        const std::string& mesh)
    {
        NiAVObject* source;
        if (IsNifPath(mesh))
            source = LoadModel(mesh);
        else
        {
            source = animData->nSceneRoot->GetObjectByName(NiFixedString(mesh.c_str()));
            if (!source)
                LogOnce(FormatString("%s: no node \"%s\" in the skeleton to copy for \"%s\" (a mesh path must end in .nif)",
                    sequence.m_kName.CStr(), mesh.c_str(), bone.name.CStr()));
        }
        if (!source)
            return nullptr;
        NiPointer<NiAVObject> object = static_cast<NiAVObject*>(ThisStdCall<NiObject*>(0xA5D680, source)); // NiObject::Clone
        if (!object)
            return nullptr;
        // the bone places it: its origin at the bone, as TESBoundObject::Clone3D places a model at its reference
        object->m_kLocal.m_Rotate = {};
        for (int i = 0; i < 3; i++)
            object->m_kLocal.m_Rotate.m_pEntry[i][i] = 1.0f;
        object->m_kLocal.m_Translate = NiPoint3::ZERO;
        object->m_kLocal.m_fScale = 1.0f;
        if (IsNifPath(mesh))
            object->m_uFlags &= ~NiAVObject::APP_CULLED_MASK;
        RemoveCollision(object);
        PrefixNames(object, std::string(bone.name.Str()) + ':');
        return object;
    }

    std::optional<Spawned> Spawn(AnimData* animData, NiControllerSequence* sequence, const SpawnKeys::Bone& bone,
        const std::string& mesh)
    {
        // added when the animation joined the skeleton (OnSequenceJoin logged why if not)
        auto* boneNode = SkeletonNodes::Find(animData->nSceneRoot, bone.name);
        if (!boneNode)
            return std::nullopt;
        auto object = CreateObject(animData, *sequence, bone, mesh);
        if (!object)
            return std::nullopt;
        SkeletonNodes::Attach(boneNode, object);
        return Spawned{ sequence, bone.name, mesh, boneNode, std::move(object), g_frame };
    }

    void Despawn(const Spawned& spawned)
    {
        auto* object = spawned.object.data;
        auto* parent = object->m_pkParent;
        if (!parent)
            return;
        // ReloadTargets fills the palette from the scene graph: forget the copy before it goes
        if (auto* manager = NiControllerManager::FindForObject(object))
            manager->ForgetObjectRecurse(object);
        parent->DetachChild(object);
    }
}

std::optional<SpawnKeys::Keys> SpawnKeys::Parse(const NiControllerSequence& sequence)
{
    if (!sequence.m_spTextKeys)
        return std::nullopt;
    Keys keys;
    for (const NiTextKey& key : sequence.m_spTextKeys->GetKeys())
    {
        std::string_view text = key.m_kText.Str();
        bool spawn;
        if (sv::starts_with_ci(text, kSpawnKey))
        {
            spawn = true;
            text.remove_prefix(kSpawnKey.size());
        }
        else if (sv::starts_with_ci(text, kDespawnKey))
        {
            spawn = false;
            text.remove_prefix(kDespawnKey.size());
        }
        else
            continue;
        const auto arguments = SplitArguments(text);
        const auto boneName = arguments[0];
        if (boneName.empty() || (spawn && (arguments.size() < 2 || arguments.size() > 3 || arguments[1].empty()))
            || (!spawn && arguments.size() != 1))
        {
            LogOnce(FormatString("%s: can't use \"%s\" (expected spawn: <bone>, <mesh>[, <parent>] or despawn: <bone>)",
                sequence.m_kName.CStr(), key.m_kText.CStr()));
            continue;
        }

        auto bone = std::ranges::find_if(keys.bones, [&](const Bone& b) { return sv::equals_ci(b.name.Str(), boneName); });
        if (bone == keys.bones.end())
        {
            // the KF's own spelling of the bone, if it animates one (node names are case sensitive)
            const auto idTags = sequence.GetIDTags();
            const auto idTag = std::ranges::find_if(idTags, [&](const NiControllerSequence::IDTag& tag)
            {
                return sv::equals_ci(tag.m_kAVObjectName.Str(), boneName);
            });
            const auto name = idTag != idTags.end() ? idTag->m_kAVObjectName : NiFixedString(std::string(boneName).c_str());
            bone = keys.bones.insert(keys.bones.end(), Bone{ name, NiFixedString(kDefaultParent) });
        }
        if (spawn && arguments.size() == 3 && !arguments[2].empty())
        {
            if (bone->parentSet && !sv::equals_ci(bone->parent.Str(), arguments[2]))
                LogOnce(FormatString("%s: \"%s\" has two parents, keeping \"%s\"", sequence.m_kName.CStr(),
                    bone->name.CStr(), bone->parent.CStr()));
            else
            {
                bone->parent = std::string(arguments[2]).c_str();
                bone->parentSet = true;
            }
        }
        keys.events.push_back({ key.m_fTime, static_cast<unsigned int>(bone - keys.bones.begin()),
            spawn ? std::string(arguments[1]) : std::string() });
    }
    if (keys.events.empty())
        return std::nullopt;
    return keys;
}

bool SpawnKeys::Has(const NiControllerSequence& sequence)
{
    const auto* extraData = SequenceExtraDatas::Get(&sequence);
    return extraData && extraData->spawnKeys;
}

void SpawnKeys::Update(AnimData* animData, NiControllerSequence* sequence, float time)
{
    const auto* extraData = SequenceExtraDatas::Get(sequence);
    if (!animData || !animData->nSceneRoot || !extraData || !extraData->spawnKeys)
        return;
    const auto& keys = *extraData->spawnKeys;
    for (unsigned int i = 0; i < keys.bones.size(); i++)
    {
        const auto& bone = keys.bones[i];
        const Event* current = nullptr;
        for (const auto& event : keys.events)
            if (event.bone == i && event.time <= time && (!current || event.time >= current->time))
                current = &event;
        if (!current || current->mesh.empty())
            continue;
        const auto iter = std::ranges::find_if(g_spawned, [&](const Spawned& spawned)
        {
            return spawned.sequence == sequence && spawned.bone == bone.name;
        });
        if (iter != g_spawned.end())
        {
            if (iter->mesh == current->mesh)
            {
                iter->frame = g_frame;
                continue;
            }
            Despawn(*iter);
            g_spawned.erase(iter);
        }
        if (auto spawned = Spawn(animData, sequence, bone, current->mesh))
            g_spawned.push_back(std::move(*spawned));
    }
}

void SpawnKeys::EndFrame()
{
    std::erase_if(g_spawned, [](const Spawned& spawned)
    {
        if (spawned.frame == g_frame)
            return false;
        Despawn(spawned);
        return true;
    });
    ++g_frame;
}

void SpawnKeys::OnSequenceJoin(NiControllerSequence& sequence)
{
    auto* manager = sequence.m_pkOwner;
    if (!sequence.m_spTextKeys || !SkeletonNodes::IsPlayerSkeleton(manager->m_pkTarget))
        return;
    auto keys = Parse(sequence);
    if (!keys)
        return;
    auto* root = static_cast<NiNode*>(manager->m_pkTarget);
    const char* kf = sequence.m_kName.CStr();
    for (const auto& bone : keys->bones)
    {
        if (SkeletonNodes::Require(manager, bone.name, bone.parent))
            continue;
        if (root->GetObjectByName(bone.name))
            LogOnce(FormatString("%s: \"%s\" names a mesh of the skeleton, not a node; use another bone name", kf,
                bone.name.CStr()));
        else if (!SkeletonNodes::Find(root, bone.parent))
            LogOnce(FormatString("%s: the parent \"%s\" of \"%s\" is not a node of the skeleton", kf, bone.parent.CStr(),
                bone.name.CStr()));
    }
    if (auto* extraData = SequenceExtraDatas::GetOrCreate(&sequence))
        extraData->spawnKeys = std::move(*keys);
}
