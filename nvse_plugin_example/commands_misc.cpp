#include "commands_misc.h"

#include "lib/clipboard/clipboardxx.hpp"

void Commands::BuildMiscCommands(const NVSECommandBuilder& builder)
{
    builder.Create("CopyToClipboard", kRetnType_Default, {ParamInfo{"text", kParamType_String, false}}, false, [](COMMAND_ARGS)
    {
        char text[0x4000];
        if (!ExtractArgs(EXTRACT_ARGS, &text))
            return true;
        clipboardxx::clipboard clipboard;
        clipboard << text;
        return true;
    }, nullptr, "Copy");
}
