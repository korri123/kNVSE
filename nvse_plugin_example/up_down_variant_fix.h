#pragma once

// Third person aim and attack groups have up and down variants (2haaimup.kf, 1hpattack8down.kf, ...) that the game blends
// in by the actor's pitch. Animation::StartGroup (0x4948C0) and Animation::SyncSequences pick the variant of a group that
// has several KFs (AnimSequenceMultiple) by name, from the playing weapon sequence's (its file path):
// AnimSequenceMultiple::GetCorrespondingSequence (0x48F500) inserts "Up"/"Down" before the first '_' of the file name,
// and without one appends it after the extension: "\2haaim.kf" became "\2haaim.kfUp", which no KF is called, so the
// variant was never played. A group gets several KFs when an actor's animation list names the same KF twice, which it
// does for every KF in more than one archive: 2haaimup/2haaimdown (Fallout - Meshes.bsa and Update.bsa: the Service
// Rifle, Assault Carbine, ... never aimed up or down in third person), 1hpattack8 up/down (Update.bsa), 2hh and
// sneak2hh aimup (Old World Blues), h2haimup (Dead Money). The name is now built before the extension when there is no
// '_' (bFixAimUpDownVariants).
namespace UpDownVariantFix
{
	void WriteHooks();
}
