// Decompiled game code.
//
// Functions reimplemented in C++ replace their original machine code through
// hooks registered here. Everything not yet decompiled still runs from
// default.xbe, so the port is playable at every step of the decompilation.
#include "../runtime/hle/hook.h"
#include "input/pad.h"

void rt::game_install_hooks()
{
    game::pad_install_hooks();
}
