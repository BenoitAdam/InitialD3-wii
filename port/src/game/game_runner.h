// Runs the checked game on the NAOMI 2 core (emu/): hands it the decrypted
// DIMM image and the BIOS service library, feeds it the controls and
// returns when the player quits. Wii build (the Windows equivalent is the
// headless runner, src/run/run_main.cpp).
#pragma once

#include "../core/bios_image.h"
#include "../core/game_session.h"
#include "core_options.h"

#include <string>

namespace idas3 {

struct RunOptions
{
    bool dynarec = true;     // SH-4 recompiler (false: interpreter)
    int presets[kCoreOptionCount];   // chosen position in each CoreOption's names[]

    RunOptions()
    {
        for (int i = 0; i < kCoreOptionCount; i++)
            presets[i] = kCoreOptions[i].defaultIndex;
    }
};

// Returns false with `error` set when the core could not start; once the
// game has run the console state is gone and the caller should exit.
bool runGame(GameSession& session, const BiosImage& bios, const RunOptions& options, std::string& error);

} // namespace idas3
