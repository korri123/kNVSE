#pragma once
#include "GameForms.h"

enum class FileAnimType
{
	Null, Weapon, Actor
};

struct FileAnimation
{
	TESForm* form;
	std::string path;
	bool firstPerson;
	FileAnimType type;


	FileAnimation(TESForm* form, std::string path, bool firstPerson, FileAnimType type)
		: form(form),
		  path(std::move(path)), firstPerson(firstPerson), type(type)
	{
	}
};

extern std::vector<FileAnimation> g_fileAnimations;

void LoadFileAnimPaths();