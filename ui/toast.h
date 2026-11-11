// toast.h -- the small always-on-top alert popup (bottom-right). showOverlay() is declared in core/events.h.
#pragma once
#include "sw_common.h"
#include "events.h"

extern HWND g_ovl;
struct Toast{ std::wstring title,text; ArgbColor accent; };
LRESULT CALLBACK OvlProc(HWND h,UINT m,WPARAM w,LPARAM l);
void toastDisplay(Toast* t);      // UI thread: takes ownership of t (called on WM_SHOWTOAST)
