// content.h -- quotes / fun facts, AFK detection and the quote cross-fade state machine.
#pragma once
#include "sw_common.h"

extern bool g_afkNow;                   // user is idle (UI thread owns this)
extern ULONGLONG g_afkEnterT0;          // GetTickCount64() when AFK started
extern std::wstring g_curQuote;         // quote currently shown (dashboard sidebar + AFK screen)
extern int g_quoteAlpha;                // 0..255 cross-fade alpha for the sidebar
extern ULONGLONG g_quoteChangeT0;       // when the current quote appeared (sidebar glitch effect)

void loadContent();                     // creates content\*.txt on first run and loads them
bool isAfk(DWORD ms);                   // no keyboard/mouse input for ms
DWORD afkThresholdMs();                 // system screensaver timeout if enabled, else 5 minutes
void updateQuoteState();                // UI thread: AFK detection + quote fades; publishes state to the AFK screen
void triggerQuoteChange();
