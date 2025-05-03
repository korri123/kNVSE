#include "sequence_extradata.h"

SequenceExtraData* SequenceExtraDatas::Get(NiControllerSequence* sequence)
{
    if (!sequence->m_spTextKeys)
        sequence->m_spTextKeys = NiTextKeyExtraData::CreateObject();
    static const NiFixedString sExtraData = "__kNVSEExtraData__";
    bool isNew;
    auto* textKeyExtraData = sequence->m_spTextKeys->GetOrAddKey(sExtraData, 0.0f, &isNew);
    if (isNew || textKeyExtraData->m_fTime == 0.0f)
    {
        auto* datum = new SequenceExtraDatas();
        textKeyExtraData->m_fTime = reinterpret_cast<float&>(datum);
    }
    auto* datum = *reinterpret_cast<SequenceExtraDatas**>(&textKeyExtraData->m_fTime);
    for (auto& iter : datum->extraData)
    {
        if (iter.first == sequence)
            return &iter.second;
    }
    return &datum->extraData.emplace_back(sequence, SequenceExtraData{}).second;
}

void SequenceExtraDatas::Delete(NiControllerSequence* sequence)
{
    static const NiFixedString sExtraData = "__kNVSEExtraData__";
    auto* textKeyExtraData = sequence->m_spTextKeys->FindFirstByName(sExtraData);
    if (!textKeyExtraData)
        return;
    auto* datum = *reinterpret_cast<SequenceExtraDatas**>(&textKeyExtraData->m_fTime);
    std::erase_if(datum->extraData, [&](auto& pair) 
    {
        return pair.first == sequence;
    });
    if (datum->extraData.empty()) 
    {
        delete datum;
        textKeyExtraData->m_fTime = 0.0f;
    }
}
