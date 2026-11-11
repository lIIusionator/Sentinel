// apple.h -- the few calls that need an Apple framework (apple.mm). sys_mac.cpp forwards to them, so that it stays libSystem-only.
#pragma once
#include <string>
#include <vector>

namespace sw{
double appleIdleSeconds();                                  // CoreGraphics: seconds since the last keyboard / mouse input
bool appleScreenLocked();                                   // CoreGraphics: the lock screen is up, or this login session is not the one on the screen (fast user switching)
std::string applePlistProgram(const std::string& plist);   // Foundation: "Program", else the first of "ProgramArguments" ("" if unreadable)
// FSEvents: onPath(path) runs on a private queue for every file or folder created or renamed inside dirs (our own writes excluded). Stop before start again.
void appleWatchStart(const std::vector<std::string>& dirs,void(*onPath)(const std::string&));
void appleWatchStop();
}
