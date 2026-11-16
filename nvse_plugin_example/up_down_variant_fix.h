#pragma once

// Third person aim and attack groups have up and down variants (2haaimup.kf, 1hpattack8down.kf, ...) that the game
// blends in by the actor's pitch. Animation::StartGroup (0x4948C0) and Animation::SyncSequences pick the variant of a
// group that has several KFs (AnimSequenceMultiple) by name, from the file path of the playing weapon sequence.
// AnimSequenceMultiple::GetCorrespondingSequence (0x48F500) inserts "Up"/"Down" before the first '_' of the file name.
// Without a '_' it appends the suffix after the extension ("\2haaim.kf" becomes "\2haaim.kfUp"), which matches no KF,
// so the variant is never played. The hook builds the name before the extension in that case (bFixAimUpDownVariants).
// A group gets several KFs when an actor's animation list names the same KF twice, which happens for every KF present
// in more than one archive, e.g. 2haaimup/2haaimdown (Fallout - Meshes.bsa and Update.bsa), 1hpattack8 up/down
// (Update.bsa), 2hh and sneak2hh aimup (Old World Blues) and h2haimup (Dead Money).
namespace UpDownVariantFix
{
	void WriteHooks();
}
