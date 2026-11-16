#pragma once

#include <utility>
#include <vector>

#include "NiNodes.h"

struct kBlendInterpItem;

// Gamebryo blends a bone from its two highest priorities: the highest takes its ease spinner's share and the next one
// takes the rest, normalized by its own weights. This is right for one switch of animations at a time but not for two
// at once on the same bones. Example: holstering while running in third person. The unequip (arm priority 55) eases out
// while the drawn weapon's run (36) is still cross fading to the unarmed run (30). The two highest priorities never see
// that, so the unequip keeps the arm until the drawn weapon's run is gone and the hand lingers at the holster.
//
// Here every sequence easing in or out belongs to a switch: the sequences an Animation eased out (from) and in (to) in
// one update, as declared where they were switched (AnimSwitch). A bone with several switches in progress is blended
// once per combination of their ends (from or to), weighted by the switches' progress. Each combination follows
// Gamebryo's rule for animations that are not easing: the highest priority takes the bone, shared by weight.
// A bone with at most one switch above its highest non-easing animation keeps Gamebryo's (and kNVSE's) blend, and each
// switch progresses at the rate that blend gives it alone.
namespace BlendTransitions
{
    // Computes the normalized weights from the switches in progress. Returns false where the regular blend applies:
    // at most one switch that matters, items kNVSE does not track, an ease no switch declared, or too many switches
    bool ComputeNormalizedWeights(const std::vector<std::pair<NiBlendInterpolator::InterpArrayItem*, kBlendInterpItem*>>& items);
}
