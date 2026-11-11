// dashboard.h -- the GDI+ control panel window.
#pragma once
#include "sw_common.h"

extern HWND g_dash;
LRESULT CALLBACK DashProc(HWND h,UINT m,WPARAM w,LPARAM l);
void openDash();
void dashInit();          // loads the avatar and device info (GDI+ must be running)
void dashShutdown();
