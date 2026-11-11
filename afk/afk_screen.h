// afk_screen.h -- the AFK screensaver: owns a dedicated render thread, its own window and the video back-ends.
#pragma once
#include "sw_common.h"
#include "afk_state.h"

void afkScreenStart();       // start the render thread (call once; cheap, returns immediately)
bool afkScreenStop();        // ask the thread to finish and wait for it (bounded). false: a controller thread had to be left behind (it may still be using GDI+ / D3D)
bool afkScreenVisible();     // true while the AFK window is on screen (the dashboard pauses its own rendering)
void afkScreenWatch();       // UI thread, once per second: replaces a render thread that is stuck inside a GPU driver call
