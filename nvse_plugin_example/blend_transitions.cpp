#include "blend_transitions.h"

#include <algorithm>

#include "anim_switch.h"
#include "blend_curves.h"
#include "blend_smoothing.h"
#include "hooks.h"
#include "utility.h"

namespace
{
    using Items = std::vector<std::pair<NiBlendInterpolator::InterpArrayItem*, kBlendInterpItem*>>;

    // a bone blends 2^kMaxSwitches combinations at most; more switches at once keep the regular blend
    constexpr unsigned int kMaxSwitches = 4;

    struct Switch
    {
        // AnimSwitch ids count per scene root
        const NiControllerManager* owner;
        UInt32 uiID;
        // the largest ease spinner and priority on this bone of the sequences it eases out (from) and in (to)
        float fFromSpinner = 0.0f;
        float fToSpinner = 0.0f;
        char cFromPriority = 0;
        char cToPriority = 0;
        bool bFrom = false;
        bool bTo = false;
        // it has an animation on this bone that can take it over: none below the highest one not easing
        bool bRelevant = false;
        // its curve was named by a key (BlendCurves): the bone shows the spinners as they are
        bool bNamedCurve = false;
        unsigned int uiBit = 0;
        float fProgress = 0.0f;
    };

    enum class Role : UInt8
    {
        None, // detached, or no interpolator
        Steady, // not easing
        From,
        To,
    };

    struct Entry
    {
        NiBlendInterpolator::InterpArrayItem* item;
        Role role = Role::None;
        int iSwitch = -1;
        float fWeight = 0.0f;
    };

    // An ease nothing declared (AnimSwitch): its bones keep the regular blend. Debug builds name the sequence once, so
    // the place that eased it can be found
    void ReportUndeclaredEase(const NiControllerSequence* sequence)
    {
#if _DEBUG
        const char* name = sequence->m_kName.CStr();
        DebugPrintOnce(FormatString("BlendTransitions: no switch declared for the ease of %s", name ? name : "(unnamed)"));
#endif
    }

    // Gamebryo's share for the highest priority easing in over a next priority at full weight
    float EaseCurve(float fSpinner)
    {
        return fSpinner * fSpinner / (fSpinner * fSpinner + 1.0f - fSpinner);
    }

    // How far the bone has switched to the switch's "to" end when it is the only switch: what the regular blend gives it
    float GetProgress(const Switch& s)
    {
        if (s.bFrom && s.bTo && s.cFromPriority == s.cToPriority)
        {
            // a cross fade at one priority is shared by spinner (ComputeNormalizedWeights' cross fade rule)
            const float fSum = s.fFromSpinner + s.fToSpinner;
            return fSum > 0.0f ? s.fToSpinner / fSum : 0.0f;
        }
        if (s.bNamedCurve)
        {
            // the higher priority takes exactly its spinner (ComputeNormalizedWeightsOfTwoHighestPriorities)
            if (!s.bFrom || (s.bTo && s.cToPriority > s.cFromPriority))
                return s.fToSpinner;
            return 1.0f - s.fFromSpinner;
        }
        if (!s.bFrom)
            return EaseCurve(s.fToSpinner);
        if (!s.bTo)
            return 1.0f - EaseCurve(s.fFromSpinner);
        // across two priorities the higher takes its spinner's share, normalized against the lower one's
        const float fSum = s.fFromSpinner * s.fFromSpinner + s.fToSpinner * s.fToSpinner;
        return fSum > 0.0f ? s.fToSpinner * s.fToSpinner / fSum : 0.0f;
    }

    bool IsEasing(NiControllerSequence::AnimState eState)
    {
        return eState == NiControllerSequence::EASEIN || eState == NiControllerSequence::EASEOUT;
    }
}

bool BlendTransitions::ComputeNormalizedWeights(const Items& items)
{
    if (!g_pluginSettings.fixOverlappingBlends || !g_pluginSettings.blendSmoothing)
        return false;

    // two switches need two sequences easing
    unsigned int uiEasing = 0;
    for (const auto& [item, extraItem] : items)
    {
        if (!item->m_spInterpolator || (extraItem && extraItem->detached))
            continue;
        if (!extraItem || !extraItem->sequence)
            return false;
        if (IsEasing(extraItem->sequence->m_eState))
            ++uiEasing;
    }
    if (uiEasing < 2)
        return false;

    thread_local std::vector<Entry> entries;
    thread_local std::vector<Switch> switches;
    entries.clear();
    switches.clear();
    bool bSteady = false;
    char cSteadyPriority = 0;
    for (const auto& [item, extraItem] : items)
    {
        Entry& entry = entries.emplace_back(item);
        if (!item->m_spInterpolator || extraItem->detached)
            continue;
        NiControllerSequence* sequence = extraItem->sequence;
        if (!IsEasing(sequence->m_eState))
        {
            entry.role = Role::Steady;
            if (item->m_fWeight > 0.0f && (!bSteady || item->m_cPriority > cSteadyPriority))
            {
                cSteadyPriority = item->m_cPriority;
                bSteady = true;
            }
            continue;
        }
        const bool bTo = sequence->m_eState == NiControllerSequence::EASEIN;
        entry.role = bTo ? Role::To : Role::From;
        const UInt32 uiSwitch = AnimSwitch::Of(sequence);
        if (!uiSwitch)
        {
            ReportUndeclaredEase(sequence);
            return false;
        }
        auto iter = ra::find_if(switches, [&](const Switch& s) { return s.owner == sequence->m_pkOwner && s.uiID == uiSwitch; });
        if (iter == switches.end())
        {
            iter = switches.insert(switches.end(), Switch{ sequence->m_pkOwner, uiSwitch });
            // every sequence of a switch takes its curve
            iter->bNamedCurve = BlendCurves::HasNamedCurve(*sequence);
        }
        entry.iSwitch = static_cast<int>(iter - switches.begin());
        auto& s = *iter;
        auto& bSide = bTo ? s.bTo : s.bFrom;
        auto& fSpinner = bTo ? s.fToSpinner : s.fFromSpinner;
        auto& cPriority = bTo ? s.cToPriority : s.cFromPriority;
        fSpinner = max(fSpinner, item->m_fEaseSpinner);
        cPriority = bSide ? max(cPriority, item->m_cPriority) : item->m_cPriority;
        bSide = true;
    }

    // a switch below the highest priority not easing never shows
    for (const auto& entry : entries)
    {
        if (entry.iSwitch != -1 && entry.item->m_fWeight > 0.0f && (!bSteady || entry.item->m_cPriority >= cSteadyPriority))
            switches[entry.iSwitch].bRelevant = true;
    }
    unsigned int uiSwitches = 0;
    for (auto& s : switches)
    {
        if (!s.bRelevant)
            continue;
        if (uiSwitches == kMaxSwitches)
            return false;
        s.uiBit = 1u << uiSwitches++;
        s.fProgress = GetProgress(s);
    }
    if (uiSwitches < 2)
        return false;

    // each combination of the switches' ends, blended as animations that are not easing: the highest priority, by weight
    float fTotal = 0.0f;
    for (unsigned int uiEnds = 0; uiEnds < (1u << uiSwitches); ++uiEnds)
    {
        float fShare = 1.0f;
        for (const auto& s : switches)
        {
            if (s.bRelevant)
                fShare *= (uiEnds & s.uiBit) ? s.fProgress : 1.0f - s.fProgress;
        }
        if (fShare <= 0.0f)
            continue;
        const auto isPresent = [&](const Entry& entry)
        {
            if (entry.item->m_fWeight <= 0.0f)
                return false;
            if (entry.role == Role::Steady)
                return true;
            if (entry.iSwitch == -1 || !switches[entry.iSwitch].bRelevant)
                return false;
            return (entry.role == Role::To) == ((uiEnds & switches[entry.iSwitch].uiBit) != 0);
        };
        bool bAny = false;
        char cTop = 0;
        for (const auto& entry : entries)
        {
            if (isPresent(entry) && (!bAny || entry.item->m_cPriority > cTop))
            {
                cTop = entry.item->m_cPriority;
                bAny = true;
            }
        }
        if (!bAny)
            continue;
        float fSum = 0.0f;
        for (const auto& entry : entries)
        {
            if (entry.item->m_cPriority == cTop && isPresent(entry))
                fSum += entry.item->m_fWeight;
        }
        for (auto& entry : entries)
        {
            if (entry.item->m_cPriority == cTop && isPresent(entry))
                entry.fWeight += fShare * entry.item->m_fWeight / fSum;
        }
        fTotal += fShare;
    }
    if (fTotal <= 0.0f)
        return false;

    for (const auto& entry : entries)
    {
        if (entry.item->m_spInterpolator)
            entry.item->m_fNormalizedWeight = entry.fWeight / fTotal;
    }
    return true;
}
