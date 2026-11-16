#pragma once
#include "GameProcess.h"
#include "NiNodes.h"

// Animation::StartGroup (0x4949A0) starts a sequence with NiControllerManager::BlendFromPose (0xA2F800) whenever its
// section has no running sequence to cross fade from. That happens for an empty slot (drawing a weapon, special idles,
// hit reactions, the Pip-Boy) or one that Animation::ClearGroup (0x496080) just emptied without an ease-out.
// BlendFromPose snapshots the bones into a temporary sequence at the new sequence's priorities with weight 1, and holds
// the new sequence at its first frame for the whole blend (TRANSDEST). Every lower priority animation on those bones
// stops for the blend, the bones move on a straight path between two still poses, and the animation starts late and
// abruptly.
// PoseBlendFixes eases the sequence in instead, so lower priorities keep playing under it and it plays from its start,
// and fades out the sequences ClearGroup cut off for it over the same time. Bones that the ragdoll poses keep the
// snapshot.
namespace PoseBlendFixes
{
    // Called before Animation::ClearGroup deactivates a sequence; records it if it is cut off without an ease-out
    void RecordHardCut(AnimData* animData, NiControllerSequence* sequence, float fEaseOut);
    void ApplyHooks();
}
