// afk_assets.h -- the AFK screen's video files: built-in clips unpacked from the exe, an optional user clip, probing.
//
// The exe carries two H.264 renditions of the background as RCDATA resources (see res/sentinel.rc):
//   AFK_4K60  3840x2160 @ 60 fps   -- tier 0 (GPU)           AFK_LITE  1920x1080 @ 30 fps -- tier 1 (GPU) and tier 2 (CPU)
// They are unpacked once into %LOCALAPPDATA%\SentinelWatch\content\afk (a media engine opens files, not memory) and the
// size is checked on every start. Anything else the user drops into that folder (mp4/mov/mkv/webm/avi/wmv/m4v) outranks
// the built-ins; such a clip is played as-is on every tier (it has no "lite" variant).
#pragma once
#include "sw_common.h"

struct AfkClip{
  std::wstring path;          // empty = not available
  std::wstring codec;         // "H264", "HEVC", ... (FOURCC of the native video type), empty if unknown
  double fps=0.0, durationSec=0.0;
  UINT32 w=0,h=0;
  bool user=false;            // a clip the user supplied
  bool valid() const { return !path.empty(); }
};

struct AfkMedia{
  AfkClip big;                // tier 0: user clip if any, else the built-in 4K 60 fps clip
  AfkClip lite;               // tier 1 (GPU) / tier 2 (CPU): built-in 1080p 30 fps clip; for a user clip: same file as `big`
  bool userClip=false;
};

// Runs on the AFK thread at start-up (COM initialised). Never throws; logs every decision. Returns true if at least one
// clip is playable.
bool afkAssetsPrepare(AfkMedia& out);
