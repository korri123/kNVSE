#pragma once

#include <utility>
#include <vector>

#include "NiNodes.h"

struct kBlendInterpItem;

// Gamebryo blends a bone from its two highest priorities: the highest takes its ease spinner's share, the next one the
// rest, normalized by its own weights. That is right for one switch of animations at a time, but not for two at once on
// the same bones: holstering while running in third person, the unequip (arm priority 55) eases out while the drawn
// weapon's run (36) is still cross fading to the unarmed run (30), which the two highest priorities never see; the
// unequip kept the arm until the drawn weapon's run was gone and the hand lingered at the holster.
//
// Here every sequence easing in or out is part of a switch: the sequences an Animation eased out (from) and in (to) in
// one update, as declared where they were switched (AnimSwitch). A bone with switches in progress is blended as each
// combination of their ends (from or to), weighted by the switches' progress, each combination by Gamebryo's rule for
// animations that are not easing: the highest priority takes the bone, shared by weight. A bone with at most one switch
// above its highest animation that is not easing keeps Gamebryo's (and kNVSE's) blend as before, and each switch
// progresses at the rate that blend gives it alone, so nothing changes where only one switch is in progress.
namespace BlendTransitions
{
    // Computes the normalized weights from the switches in progress, or returns false where the regular blend applies
    // (at most one switch that matters, items kNVSE does not track, an ease no switch declared, or too many switches)
    bool ComputeNormalizedWeights(const std::vector<std::pair<NiBlendInterpolator::InterpArrayItem*, kBlendInterpItem*>>& items);
}
