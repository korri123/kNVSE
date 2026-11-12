#pragma once

#include <optional>
#include <string>
#include <vector>

#include "NiNodes.h"

struct AnimData;

// Text keys that show a mesh for part of an animation without editing any mesh or skeleton (a second magazine in a
// reload, a tool in the hand, ...):
//   "spawn: <bone>, <mesh>[, <parent>]"   from this key on, a copy of <mesh> is shown at <bone>
//   "despawn: <bone>"                     from this key on, it isn't
// <mesh> is a NIF under Data\Meshes ("Weapons\2HandAutomatic\ServiceRifle.nif"), or the name of a node of the actor's 3D
// to copy ("##Clip", the Service Rifle's magazine, ...). <bone> is a node kNVSE adds to the skeleton, as a child of
// <parent> (default "Weapon"), when the animation is added to it (its first play): the KF animates it by that name like
// any bone, relative to <parent>, so in the rig it is a bone of that name under <parent> with the mesh on it at no
// offset (the mesh's origin is the bone). Without a track for <bone> in the KF the mesh sits at <parent>'s origin.
// <bone> can also be a node the skeleton already has ("Bip01 L Hand"): then nothing is added and the mesh is attached to
// that node. What is shown follows the animation's time: the latest of the bone's spawn/despawn keys at or before it,
// nothing before the first. So loops and restarts show it again, and it goes away when the animation stops or is
// interrupted. Player only (first and third person skeletons): other actors play the animation without the mesh.
// Node names in the copy get "<bone>:" in front, so they never shadow the names the KFs and the engine look up.
//
// The bone is in the skeleton before the engine binds the KF (NiControllerSequence::StoreTargets), so the engine animates
// it like any other: SkeletonNodes adds it and keeps it through target reloads. The keys are parsed once, into the
// sequence's SequenceExtraData.
namespace SpawnKeys
{
    struct Bone
    {
        NiFixedString name;
        NiFixedString parent;
        bool parentSet = false;
    };

    struct Event
    {
        float time;
        unsigned int bone;
        // empty: despawn
        std::string mesh;
    };

    struct Keys
    {
        std::vector<Bone> bones;
        std::vector<Event> events;
    };

    // The spawn/despawn keys of a sequence; nullopt if it has none
    std::optional<Keys> Parse(const NiControllerSequence& sequence);

    // A sequence joins a controller manager, before it is bound to the skeleton's nodes (SequenceExtraDatas' hook): keeps
    // the keys of a sequence of the player with it and gives the skeleton their bones
    void OnSequenceJoin(NiControllerSequence& sequence);

    // Whether the sequence joined its manager with keys
    bool Has(const NiControllerSequence& sequence);

    // Main thread, once per frame for each playing animation of the player that has keys, with its time
    void Update(AnimData* animData, NiControllerSequence* sequence, float time);

    // Main thread, after Update ran for every playing animation: removes what animations no longer show
    void EndFrame();
}
