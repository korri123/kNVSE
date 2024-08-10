#include "AnimDataHooks.h"
#include "game_types.h"
#include "NiObjects.h"
#include "SafeWrite.h"

// Local types
struct TES;

using AnimGroups = UInt16;

enum MenuCode
{
  Console = 0x3,
  Message = 0x3E9,
  Inventory = 0x3EA,
  Stats = 0x3EB,
  HUDMainMenu = 0x3EC,
  Loading = 0x3EF,
  Container = 0x3F0,
  Dialog = 0x3F1,
  SleepWait = 0x3F4,
  Pause = 0x3F5,
  LockPick = 0x3F6,
  Quantity = 0x3F8,
  PipboyData = 0x3FF,
  LevelUp = 0x403,
  PipboyRepair = 0x40B,
  SurgeryMenu = 0x40C,
  Credits = 0x417,
  CharGen = 0x418,
  TextEdit = 0x41B,
  Barter = 0x41D,
  Surgery = 0x41E,
  Hacking = 0x41F,
  VATS = 0x420,
  Computers = 0x421,
  VendorRepair = 0x422,
  Tutorial = 0x423,
  SPECIALBook = 0x424,
  ItemModMenu = 0x425,
  LoveTester = 0x432,
  CompanionWheel = 0x433,
  MedicalQuestionnaire = 0x434,
  Recipe = 0x435,
  SlotMachine = 0x438,
  BlackJack = 0x439,
  Roulette = 0x43A,
  Caravan = 0x43B,
  Traits = 0x43C,
  VideoMenu = 0x3FA,
  GamePlayMenu = 0x3FC,
  BookMenu = 0x402,
  AudioMenu = 0x3F9,
};

struct VATSCameraData;

using HighProcess = Decoding::HighProcess;

// Function declarations
const auto InterfaceManager_GetIsMenuMode_ = reinterpret_cast<bool(__cdecl *)()>(0x702360);
const auto PlayerCharacter_GetNode = reinterpret_cast<NiNode*(__thiscall *)(PlayerCharacter *_this, bool a2)>(0x950bb0);
const auto InterfaceManager_IsPipboyMode = reinterpret_cast<bool(__cdecl *)()>(0x705a00);
const auto AnimData_ResetSequenceState = reinterpret_cast<void(__thiscall *)(AnimData *_this, eAnimSequence seqID, float blendAmount)>(0x496080);
const auto BSCoreMessage_Warning = reinterpret_cast<int(__cdecl *)(const char *, ...)>(0x5b5e40);
const auto TESAnimGroup_GetMaxOfBlend_BlendOut = reinterpret_cast<UInt8(__thiscall *)(TESAnimGroup *_this)>(0x495520);
const auto TESAnimGroup_GetMaxOfBlend_BlendIn = reinterpret_cast<UInt8(__thiscall *)(TESAnimGroup *_this)>(0x4954e0);
const auto PlayerCharacter_InventoryMenu = reinterpret_cast<NiNode*(__thiscall *)(PlayerCharacter *_this)>(0x490f80);
const auto NiPointer_IsEqual = reinterpret_cast<bool(__thiscall *)(void *_this, NiRefObject *)>(0x822510);
const auto IsMenuActive = reinterpret_cast<char(__cdecl *)(MenuCode, int)>(0x702680);
const auto TES_IsRunningCellTests = reinterpret_cast<bool(__thiscall *)(TES *_this)>(0x451530);
const auto Get_bFootIK = reinterpret_cast<bool(__cdecl *)()>(0x495580);
const auto bhkRagdollController_4955A0 = reinterpret_cast<char(__thiscall *)(bhkRagdollController *_this)>(0x4955a0);
const auto bhkRagdollController_ApplyBoneTransforms = reinterpret_cast<void(__thiscall *)(bhkRagdollController *_this)>(0xc75b40);


// Globals
auto* g_VATSCameraData = reinterpret_cast<TESForm*>(0x11f2250);
auto* gs_fMenuModeAnimBlend_Interface = reinterpret_cast<SettingT*>(0x11c5740);
auto* g_TES = reinterpret_cast<TES*>(0x11dea10);
auto* gs_fAnimationDefaultBlend_General = reinterpret_cast<SettingT*>(0x11c56fc);
auto* gs_fAnimationMult_General = reinterpret_cast<SettingT*>(0x11c5724);


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
BSAnimGroupSequence *AnimData::MorphOrBlendToSequence(BSAnimGroupSequence *apDestSequence,
        UInt16 usAnimGroup,
        eAnimSequence aSequenceType)
{
  auto* pPlayer = g_thePlayer;
  
  if ( IsAnimSequenceQueued(apDestSequence) )
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

  BSAnimGroupSequence* currentSequence = this->animSequence[eSequenceType];
  const auto baseGroupId = static_cast<AnimGroupID>(this->groupIDs[eSequenceType] & 0xff);
  bool shouldMorph = false;
  auto currentSequenceState = NiControllerSequence::INACTIVE;

  if ( currentSequence )
    currentSequenceState = currentSequence->m_eState;
  if ( !apDestSequence || usAnimGroup == 0xff )
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
    apDestSequence->Deactivate(0.0, 0);
  if ( (apDestSequence->m_eState && apDestSequence->m_eCycleType != NiControllerSequence::LOOP
     || apDestSequence->m_eState == NiControllerSequence::ANIMATING && apDestSequence == currentSequence)
    && (!InterfaceManager_GetIsMenuMode_() || apDestSequence->m_eState == NiControllerSequence::ANIMATING) )
  {
    this->sequenceState1[eSequenceType] = 0;
    apDestSequence->m_fOffset = -NI_INFINITY;
    return apDestSequence;
  }
  if ( !InterfaceManager_GetIsMenuMode_()
    || this->nSceneRoot != PlayerCharacter_GetNode(pPlayer, false)
    || g_VATSCameraData->flags )
  {
    if ( !InterfaceManager_IsPipboyMode()
      || this->nSceneRoot != PlayerCharacter_GetNode(pPlayer, true)
      || groupId != kAnimGroup_DynamicIdle
      || aSequenceType != kSequence_Idle
      || !currentSequence )
    {
      if ( !currentSequence || eSequenceType != kSequence_LeftArm )
      {
        if ( currentSequenceState != NiControllerSequence::INACTIVE && currentSequence )
        {
          if ( currentSequenceState == NiControllerSequence::ANIMATING )
          {
            if ( eOriginalSequenceId != eSequenceType || this->noBlend120 || apDestSequence->m_eState )
            {
              AnimData_ResetSequenceState(this, eOriginalSequenceId, 0.0);
            }
            else if ( eOriginalSequenceId == kSequence_Weapon
                   && (groupId <= kAnimGroup_Unequip || groupId > kAnimGroup_JamM)
                   && !AnimGroup::IsAim(groupId) )
            {
              AnimData_ResetSequenceState(this, kSequence_WeaponUp, 0.0);
              AnimData_ResetSequenceState(this, kSequence_WeaponDown, 0.0);
            }
          }
          else
          {
            if ( this->actor != pPlayer && eSequenceType == kSequence_Movement )
              return nullptr;
            AnimData_ResetSequenceState(this, eOriginalSequenceId, 0.0);
            currentSequence = nullptr;
          }
        }
      }
      else
      {
        const auto currGroupId = currentSequence->animGroup->GetBaseGroupID();
        if ( groupId == kAnimGroup_PipBoy
          || groupId == kAnimGroup_PipBoyChild
          && currGroupId >= kAnimGroup_HandGrip1
          && currGroupId <= kAnimGroup_HandGrip6 )
        {
          AnimData_ResetSequenceState(this, eOriginalSequenceId, 0.0);
          currentSequence = nullptr;
        }
      }
    }
    else
    {
      AnimData_ResetSequenceState(this, eOriginalSequenceId, 0.0);
      currentSequence = nullptr;
      this->noBlend120 = true;
    }
  }
  this->groupIDs[eSequenceType] = usAnimGroup;
  this->animSequence[eSequenceType] = apDestSequence;
  if ( !InterfaceManager_GetIsMenuMode_()
    || this->nSceneRoot != PlayerCharacter_GetNode(pPlayer, false) )
  {
    if ( currentSequence )
    {
      if ( currentSequence->animGroup->leftOrRight_whichFootToSwitch )
      {
        if ( currentSequence->animGroup->leftOrRight_whichFootToSwitch == apDestSequence->animGroup->leftOrRight_whichFootToSwitch )
        {
          if ( currentSequence->m_uiArraySize == apDestSequence->m_uiArraySize )
          {
            if ( apDestSequence->CanSyncTo(currentSequence) )
            {
              shouldMorph = true;
            }
            else
            {
              BSCoreMessage_Warning(
                "ANIMATION: Morph Error - Morph tags are different in '%s' and '%s' for '%s'.",
                currentSequence->m_kName,
                apDestSequence->m_kName,
                nSceneRoot->m_pcName);
            }
          }
          else
          {
            BSCoreMessage_Warning(
              "ANIMATION: Morph Error - Controller count not the same.\r\n"
              "'%s' has %d controllers and\r\n"
              "'%s' has %d on '%s'.",
              currentSequence->m_kName,
              currentSequence->m_uiArraySize,
              apDestSequence->m_kName,
              apDestSequence->m_uiArraySize,
              nSceneRoot->m_pcName);
          }
        }
      }
      if ( shouldMorph )
      {
        if ( apDestSequence == currentSequence )
        {
          BSCoreMessage_Warning(
            "ANIMATION: Morph Error - Trying to morph from sequence to itself.\r\n'%s' on '%s'.",
            currentSequence->m_kName,
            nSceneRoot->m_pcName);
          shouldMorph = false;
        }
      }
    }
  }
  float animationBlend = gs_fAnimationDefaultBlend_General->GetFloatValue()->f;
  UInt8 maxOfBlend_BlendOut = 0;
  if ( currentSequence )
  {
    maxOfBlend_BlendOut = TESAnimGroup_GetMaxOfBlend_BlendOut(currentSequence->animGroup);
  }
  const auto maxOfBlend_BlendIn = TESAnimGroup_GetMaxOfBlend_BlendIn(apDestSequence->animGroup);
  if ( maxOfBlend_BlendIn > maxOfBlend_BlendOut )
  {
    maxOfBlend_BlendOut = TESAnimGroup_GetMaxOfBlend_BlendIn(apDestSequence->animGroup);
  }
  if ( maxOfBlend_BlendOut )
    animationBlend = static_cast<float>(maxOfBlend_BlendOut) / 30.0f;
  if ( InterfaceManager_GetIsMenuMode_()
    && this->nSceneRoot == pPlayer->spInventoryMenu
    || IsMenuActive(SurgeryMenu, 0) )
  {
    animationBlend = gs_fMenuModeAnimBlend_Interface->GetFloatValue()->f;
  }
  if ( groupId >= kAnimGroup_ReloadA && groupId <= kAnimGroup_ReloadZ && currentSequence )
  {
    if ( this->actor != pPlayer && animationBlend < 0.5 )
    {
      if ( apDestSequence->animGroup->GetMoveType() == 1 && currentSequence->animGroup->GetMoveType() != 1
        || currentSequence->animGroup->GetMoveType() == 1 )
      {
        animationBlend = 0.5;
      }
    }
  }
  if ( AnimGroup::IsAttack(baseGroupId) )
  {
    if ( groupId >= kAnimGroup_ReloadWStart && groupId <= kAnimGroup_ReloadZ && this->actor == pPlayer )
      animationBlend = 0.0;
  }
  if ( this->noBlend120 )
    animationBlend = 0.0;
  auto* fAnimationMult = gs_fAnimationMult_General->GetFloatValue();
  animationBlend = animationBlend / fAnimationMult->f;
  apDestSequence->SetTimePassed(0.0, false);
  if ( !currentSequence && (eSequenceType == kSequence_WeaponUp || eSequenceType == kSequence_WeaponDown) )
  {
    this->controllerManager->ActivateSequence(apDestSequence, 0, true, apDestSequence->m_fSeqWeight, animationBlend, nullptr);
  }
  else if ( animationBlend >= 0.01f )
  {
    if ( shouldMorph )
    {
      this->controllerManager->Morph(currentSequence, apDestSequence, animationBlend, 0, currentSequence->m_fSeqWeight, apDestSequence->m_fSeqWeight);
    }
    else
    {
      bool crossFadeSuccess = false;
      if ( currentSequence && currentSequence->m_eState != NiControllerSequence::INACTIVE )
      {
        crossFadeSuccess = this->controllerManager->CrossFade(currentSequence, apDestSequence,
          animationBlend, 0, false, apDestSequence->m_fSeqWeight, nullptr);
      }
      if ( !crossFadeSuccess )
      {
        if ( TES_IsRunningCellTests(g_TES) )
        {
          this->controllerManager->ActivateSequence(apDestSequence, 0, true, 1.0, 0.0, nullptr);
        }
        else
        {
          if ( this->actor && this->actor->IsActor() && Get_bFootIK() &&
            this->actor->ragDollController && bhkRagdollController_4955A0(this->actor->ragDollController) )
          {
            bhkRagdollController_ApplyBoneTransforms(this->actor->ragDollController);
          }
          this->controllerManager->BlendFromPose(apDestSequence, 0.0, animationBlend,
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
  if ( currentSequence && currentSequence != apDestSequence )
  {
    const auto currSeqState = currentSequence->m_eState;
    if ( currSeqState > NiControllerSequence::INACTIVE && (currSeqState <= NiControllerSequence::EASEIN || currSeqState == NiControllerSequence::TRANSDEST) )
    {
      this->controllerManager->DeactivateSequence(currentSequence, 0.0);
    }
  }
  this->sequenceState1[eSequenceType] = 0;
  return apDestSequence;
}

void AnimDataHooks::WriteHooks()
{
  WriteRelJump(0x4949A0, &AnimData::MorphOrBlendToSequence);
}