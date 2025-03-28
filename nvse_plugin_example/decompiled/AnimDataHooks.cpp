#include "AnimDataHooks.h"

#include <GameUI.h>

#include "game_types.h"
#include "NiObjects.h"
#include "GameProcess.h"

using HighProcess = Decoding::HighProcess;

bool AnimData::IsAnimSequenceQueued(const BSAnimGroupSequence* apSequence) const
{
  if (this->idleAnim)
  {
    if (this->idleAnim->animSequence018 == apSequence)
      return true;
  }
  else if (this->idleAnimQueued)
  {
    if (this->idleAnimQueued->animSequence018 == apSequence)
      return true;
  }
  else if (this->spIdle12C)
  {
    if (this->spIdle12C->animSequence018 == apSequence)
      return true;
  }
  else if (this->spIdle130)
    if (this->spIdle130->animSequence018 == apSequence)
      return true;
  return false;
}

// Function
BSAnimGroupSequence *AnimData::MorphOrBlendToSequence(BSAnimGroupSequence *apDestSequence, UInt16 usAnimGroup, eAnimSequence aSequenceType)
{
#if 1
  return ThisStdCall<BSAnimGroupSequence*>(0x4949A0, this, apDestSequence, usAnimGroup, aSequenceType);
#else
  
  const auto* pPlayer = PlayerCharacter::GetSingleton();
  
  if ( this->IsAnimSequenceQueued(apDestSequence) )
    return nullptr;

  auto eSequenceType = aSequenceType;
  const auto groupId = static_cast<AnimGroupID>(usAnimGroup);
  if ( eSequenceType == -1 )
    eSequenceType = GetSequenceType(groupId);
  const auto eOriginalSequenceId = eSequenceType;
  if ( eSequenceType == kSequence_Death )
  {
    eSequenceType = kSequence_Movement;
  }
  else if ( eSequenceType == (kSequence_Death|kSequence_Movement) )
  {
    eSequenceType = kSequence_Weapon;
  }

  BSAnimGroupSequence* pCurrentSequence = this->animSequence[eSequenceType];
  const auto baseGroupId = static_cast<AnimGroupID>(this->groupIDs[eSequenceType] & 0xff);
  bool shouldMorph = false;
  auto currentSequenceState = NiControllerSequence::INACTIVE;

  if ( pCurrentSequence )
    currentSequenceState = pCurrentSequence->m_eState;
  if ( !apDestSequence || usAnimGroup == 0xffff )
    return nullptr;
  if ( eSequenceType == kSequence_Weapon
    || eSequenceType == kSequence_WeaponUp
    || eSequenceType == kSequence_WeaponDown )
  {
    if ( this->actor == pPlayer && this == pPlayer->GetAnimData(true) )
    {
      float weight;
      if ( eSequenceType == kSequence_Weapon )
        weight = 1.0;
      else
        weight = 0.0;
      apDestSequence->m_fSeqWeight = weight;
    }
    else
    {
      const auto* highProcess = this->actor->baseProcess;
      bool shouldSetWeight = true;
      if ( AnimGroup::IsAim(groupId) || AnimGroup::IsAttack(groupId) )
      {
        if ( highProcess )
        {
          if ( highProcess->processLevel <= 1 )
          {
            if ( const auto *nthWeaponSequence = highProcess->weaponSequence[eSequenceType - 4] )
            {
              apDestSequence->m_fSeqWeight = nthWeaponSequence->m_fSeqWeight;
              shouldSetWeight = false;
            }
          }
        }
      }
      if ( shouldSetWeight )
      {
        if ( eSequenceType == kSequence_WeaponUp || eSequenceType == kSequence_WeaponDown )
          apDestSequence->m_fSeqWeight = 0.0;
      }
    }
  }
  if ( apDestSequence->m_eState == NiControllerSequence::EASEOUT )
    apDestSequence->Deactivate(0.0, false);
  if ( (apDestSequence->m_eState && apDestSequence->m_eCycleType != NiControllerSequence::LOOP
     || apDestSequence->m_eState == NiControllerSequence::ANIMATING && apDestSequence == pCurrentSequence)
    && (!InterfaceManager::IsMenuMode() || apDestSequence->m_eState == NiControllerSequence::ANIMATING) )
  {
    this->sequenceState1[eSequenceType] = 0;
    apDestSequence->m_fOffset = -NI_INFINITY;
    return apDestSequence;
  }
  TESAnimGroup* pCurrentAnimGroup = pCurrentSequence ? pCurrentSequence->animGroup : nullptr;
  if ( !InterfaceManager::IsMenuMode()
    || this->nSceneRoot != pPlayer->GetNode(false)
    || VATSCameraData::GetSingleton()->eMode != VATSCameraData::VATS_MODE_NONE )
  {
    if ( !InterfaceManager::IsPipboyMode()
      || this->nSceneRoot != pPlayer->GetNode(true)
      || groupId != kAnimGroup_DynamicIdle
      || aSequenceType != kSequence_Idle
      || !pCurrentSequence )
    {
      if ( !pCurrentSequence || eSequenceType != kSequence_LeftArm )
      {
        if ( currentSequenceState != NiControllerSequence::INACTIVE && pCurrentSequence )
        {
          if ( currentSequenceState == NiControllerSequence::ANIMATING )
          {
            if ( eOriginalSequenceId != eSequenceType || this->noBlend120 || apDestSequence->m_eState )
            {
              this->ResetSequenceState(eOriginalSequenceId, 0.0);
            }
            else if ( eOriginalSequenceId == kSequence_Weapon
                   && (groupId <= kAnimGroup_Unequip || groupId > kAnimGroup_JamM)
                   && !AnimGroup::IsAim(groupId) )
            {
              this->ResetSequenceState(kSequence_WeaponUp, 0.0);
              this->ResetSequenceState(kSequence_WeaponDown, 0.0);
            }
          }
          else
          {
            if ( this->actor != pPlayer && eSequenceType == kSequence_Movement )
              return nullptr;
            this->ResetSequenceState(eOriginalSequenceId, 0.0);
            pCurrentSequence = nullptr;
          }
        }
      }
      else
      {
        const auto currGroupId = pCurrentAnimGroup->GetBaseGroupID();
        if ( groupId == kAnimGroup_PipBoy
          || groupId == kAnimGroup_PipBoyChild
          && currGroupId >= kAnimGroup_HandGrip1
          && currGroupId <= kAnimGroup_HandGrip6 )
        {
          this->ResetSequenceState(eOriginalSequenceId, 0.0);
          pCurrentSequence = nullptr;
        }
      }
    }
    else
    {
      this->ResetSequenceState(eOriginalSequenceId, 0.0);
      pCurrentSequence = nullptr;
      this->noBlend120 = true;
    }
  }
  this->groupIDs[eSequenceType] = usAnimGroup;
  this->animSequence[eSequenceType] = apDestSequence;
  const TESAnimGroup* pDestAnimGroup = apDestSequence->animGroup;
  if ( (!InterfaceManager::IsMenuMode()
    || this->nSceneRoot != pPlayer->GetNode(false)) && pCurrentSequence )
  {
    if ( pCurrentAnimGroup->leftOrRight_whichFootToSwitch &&
      pCurrentAnimGroup->leftOrRight_whichFootToSwitch == pDestAnimGroup->leftOrRight_whichFootToSwitch )
    {
      if ( pCurrentSequence->m_uiArraySize == apDestSequence->m_uiArraySize )
      {
        if ( apDestSequence->CanSyncTo(pCurrentSequence) )
        {
          shouldMorph = true;
        }
        else
        {
          BSCoreMessage::Warning(
            "ANIMATION: Morph Error - Morph tags are different in '%s' and '%s' for '%s'.",
            pCurrentSequence->m_kName,
            apDestSequence->m_kName,
            nSceneRoot->m_pcName);
        }
      }
      else
      {
        BSCoreMessage::Warning(
          "ANIMATION: Morph Error - Controller count not the same.\r\n"
          "'%s' has %d controllers and\r\n"
          "'%s' has %d on '%s'.",
          pCurrentSequence->m_kName,
          pCurrentSequence->m_uiArraySize,
          apDestSequence->m_kName,
          apDestSequence->m_uiArraySize,
          nSceneRoot->m_pcName);
      }
    }
    if ( shouldMorph && apDestSequence == pCurrentSequence )
    {
      BSCoreMessage::Warning(
        "ANIMATION: Morph Error - Trying to morph from sequence to itself.\r\n'%s' on '%s'.",
        pCurrentSequence->m_kName,
        nSceneRoot->m_pcName);
      shouldMorph = false;
    }
  }
  float easeInTime = GameSettings::General::fAnimationDefaultBlend->GetFloatValue();
  UInt8 blend = 0;
  if ( pCurrentSequence )
  {
    blend = pCurrentAnimGroup->GetBlendOut();
  }
  const auto blendIn = pDestAnimGroup->GetBlendIn();
  if ( blendIn > blend )
  {
    blend = blendIn;
  }
  if ( blend )
    easeInTime = static_cast<float>(blend) / 30.0f;

  if ( InterfaceManager::IsMenuMode() && this->nSceneRoot == pPlayer->spInventoryMenu
    || InterfaceManager::IsMenuActive(SurgeryMenu, 0) )
  {
    easeInTime = GameSettings::Interface::fMenuModeAnimBlend->GetFloatValue();
  }
  if ( groupId >= kAnimGroup_ReloadA && groupId <= kAnimGroup_ReloadZ && pCurrentSequence )
  {
    if ( this->actor != pPlayer && easeInTime < 0.5 )
    {
      if ( pDestAnimGroup->GetMoveType() == 1 && pCurrentAnimGroup->GetMoveType() != 1
        || pCurrentAnimGroup->GetMoveType() == 1 )
      {
        easeInTime = 0.5;
      }
    }
  }
  if ( AnimGroup::IsAttack(baseGroupId) )
  {
    if ( groupId >= kAnimGroup_ReloadWStart && groupId <= kAnimGroup_ReloadZ && this->actor == pPlayer )
      easeInTime = 0.0;
  }
  if ( this->noBlend120 )
    easeInTime = 0.0;
  const auto fAnimationMult = GameSettings::General::fAnimationMult->GetFloatValue();
  easeInTime = easeInTime / fAnimationMult;
  apDestSequence->SetTimePassed(0.0, false);
  if ( !pCurrentSequence && (eSequenceType == kSequence_WeaponUp || eSequenceType == kSequence_WeaponDown) )
  {
    this->controllerManager->ActivateSequence(apDestSequence, 0, true, apDestSequence->m_fSeqWeight, easeInTime, nullptr);
  }
  else if ( easeInTime >= 0.01f )
  {
    if ( shouldMorph )
    {
      this->controllerManager->Morph(pCurrentSequence, apDestSequence, easeInTime, 0, pCurrentSequence->m_fSeqWeight, apDestSequence->m_fSeqWeight);
    }
    else
    {
      bool crossFadeSuccess = false;
      if ( pCurrentSequence && pCurrentSequence->m_eState != NiControllerSequence::INACTIVE )
      {
        crossFadeSuccess = this->controllerManager->CrossFade(pCurrentSequence, apDestSequence,
          easeInTime, 0, true, apDestSequence->m_fSeqWeight, nullptr);
      }
      if ( !crossFadeSuccess )
      {
        if ( TES::GetSingleton()->IsRunningCellTests() )
        {
          this->controllerManager->ActivateSequence(apDestSequence, 0, true, 1.0, 0.0, nullptr);
        }
        else
        {
          if ( this->actor && this->actor->IsActor() && INISettings::UseRagdollAnimFootIK() &&
            this->actor->ragDollController && this->actor->ragDollController->bHasFootIK )
          {
            this->actor->ragDollController->ApplyBoneTransforms();
          }
          this->controllerManager->BlendFromPose(apDestSequence, 0.0, easeInTime,
            0, nullptr);
        }
      }
    }
  }
  else
  {
    this->controllerManager->ActivateSequence(apDestSequence, 0, true, apDestSequence->m_fSeqWeight,
      0.0f, nullptr);
  }
  if ( pCurrentSequence && pCurrentSequence != apDestSequence )
  {
    const auto currSeqState = pCurrentSequence->m_eState;
    if ( currSeqState > NiControllerSequence::INACTIVE && (currSeqState <= NiControllerSequence::EASEIN || currSeqState == NiControllerSequence::TRANSDEST) )
    {
      this->controllerManager->DeactivateSequence(pCurrentSequence, 0.0);
    }
  }
  this->sequenceState1[eSequenceType] = 0;
  return apDestSequence;
#endif
}

void AnimDataHooks::WriteHooks()
{
}