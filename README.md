# Sentinel Watch Suite

A native Windows (C++17, 64-bit) file-change / process / autorun / token-theft monitor with a GDI+ dashboard and
an **AFK screensaver**: a looping 4K 60 fps video with sound, a starfield, a camera-lens vignette, a clock and a quote.

One statically linked exe, **both AFK video clips built into it** (they unpack themselves on first use).
When it flags a program it also reads the program's strings for links and malware commands (section 10, on by default). It **starts with Windows, hidden in the tray**
(the `autostart` switch, section 4) and **updates itself from this repository's releases** (section 13). The same suite also exists as a **macOS app** (menu bar, dashboard, AFK screen; `mac/`, section 12) and as a small macOS command-line
version of the strings check (section 11). The build checks which OS it is running on and builds the matching one (section 1).

Version v1.0, build id `2026.11.11-v1.0` (`shared/version.h`). License: MIT (`LICENSE`); the two video clips and the avatar picture are **not** covered by it (`THIRD_PARTY.md`).

## Download and install (Windows)

Take the files from the [latest release](https://github.com/lIIusionator/Sentinel/releases/latest):

| File | What it is |
|---|---|
| `SentinelWatch-Setup.exe` | the installer. For this user only, **no administrator rights**: it installs to `%LOCALAPPDATA%\Programs\SentinelWatch`, adds a Start menu entry and an uninstaller (Settings -> Apps) and starts the program |
| `SentinelWatch.exe` | the same program as one file: put it in any folder you can write to and run it (nothing is installed) |
| `SHA256SUMS.txt` | the SHA-256 of the two Windows files (the updater checks the one of `SentinelWatch.exe`; you can too: `certutil -hashfile SentinelWatch.exe SHA256`) |
| `SentinelWatch-macOS.dmg` | the Mac app, **only on releases where the Mac build worked** (it has never been run on a Mac: section 12) |

The files are **not code-signed** (no certificate): Windows SmartScreen shows "Windows protected your PC" the first time; "More info" -> "Run anyway". After that the program keeps itself
up to date (section 13) and starts with Windows, hidden in the tray (double-click the tray icon to open it).

---------------------------------------------------------------------------------------------------------------------

## 1. Build

You need MinGW-w64, 64-bit (the reference toolchain is GCC 16.1, `x86_64-msvcrt-posix-seh`). `g++` and `windres` must be on `PATH`.

```bat
:: Windows, from this folder
mingw32-make                :: sentinel.exe          (about 81 MB: the two clips are inside)
mingw32-make TEST=1         :: sentinel_test.exe     (same code + environment switches for smoke tests; never ship this one)
build.bat                   :: the same without make (build.bat test for the test build)
build_portable.bat          :: no compiler installed? downloads a portable one (once) and builds, see "Build without installing a compiler"
```

```sh
# Linux cross-compile
./build.sh                  # sentinel.exe       (build.sh TEST=1 -> sentinel_test.exe)
# macOS
./build.sh                  # build-mac/SentinelWatch.app   (section 12)
```

**Which OS is this?** It is checked before anything is compiled, and the OS decides what gets built:

| Build run on | Builds | With |
|---|---|---|
| Windows (`mingw32-make`, `build.bat`, `build_portable.bat`) | `sentinel.exe` | MinGW-w64 (above) |
| macOS (`./build.sh` or `make`) | `build-mac/SentinelWatch.app` | Apple clang from the Xcode command line tools (`xcode-select --install`), `mac/mac.mk`; section 12 |
| Linux (`./build.sh`) | `sentinel.exe`, cross-compiled | `x86_64-w64-mingw32-g++`; the Mac app needs a Mac |
| anything else | nothing | stops with "Unsupported OS" |

The Makefile looks at `OS` / `uname -s` first (it prints `[OS] <name>`), and `build.sh` does the same and, on a Mac, checks that Apple clang is installed before it starts.
The sources check too (`shared/platform.h`, included by `shared/sw_common.h` and `mac/core.cpp`): compiling a tree on the wrong OS stops with one clear `#error` instead of
a page of "windows.h not found".

* **The two video clips must be in `res\` before you build.** They are part of this repository (78 MB, so a `git clone` takes a moment; see "The two clips" below).
  `make` and `build.bat` stop with a clear message when they are missing.
* The first build is slow in the resource step (`windres` embeds 77 MB of video, about 1 GB of RAM). After that `make`
  only recompiles what changed (header dependencies are tracked) and reuses the resource object.
* Only C++17 is required; everything also compiles clean as C++20 and C++23 (checked with `-fsyntax-only`).
* Static imports are Win32 + GDI+ + Media Foundation + GDI32/WINMM. Direct3D, DXGI, Direct2D, DirectWrite and avrt are loaded
  **at run time**, so the exe still starts (and falls back to the CPU renderer) on a PC without a working GPU stack.

| Command | What it builds / runs |
|---|---|
| `make` | `sentinel.exe` |
| `make TEST=1` | `sentinel_test.exe` (compiled with `-DSW_TEST_HOOKS`) |
| `make dll` | `media_engine.dll`, the GPU player on its own behind the C ABI of `media-engine/include/media_engine.h` |
| `make tests` | native unit tests of the pure logic, the autostart rule, the updater's rules, the Mac app's portable core and the Mac command-line tool (host `g++` and `bash`, not the cross compiler; on a Mac `make tests` runs the Mac-side ones with Apple clang) |
| `make macarm` | the Mac command-line tool cross-built for Apple Silicon with Zig (`ZIG=path\to\zig`); on a Mac see section 11 |
| `make run` | on a Mac only: builds `SentinelWatch.app` and opens it |
| `make harness` | `harness.exe`: the CPU renderer's decode worker against a scripted fake video reader (Windows or Wine) |
| `make pathtest` | `path_unicode_test.exe`: data folder / log / settings under accented, Vietnamese, Japanese and emoji account names |
| `make idletest` | `idle_cpu_test.exe`: the signed-file cache of the signature check and the `[CPU]` log line (Windows or Wine) |
| `make clean` | removes build output |
| `tests/update_e2e.sh <old.exe> <new.exe>` | not a make target: the automatic update end to end under Wine, against a fake release server (section 13) |
| `tests/installer_test.sh` | not a make target: the Windows setup under Wine, silent install and uninstall (needs Wine with 32-bit support: the setup is a 32-bit program) |

### The two clips

`res\afk_bg_4k60.mp4` (65.5 MB: 3840x2160, 60 fps, H.264 + AAC) and `res\afk_bg_lite.mp4` (12.4 MB: 1920x1080, 30 fps) are what `windres`
embeds. **Both are in this repository: clone, build, and there is nothing else to fetch.** (They are not covered by the MIT license: `THIRD_PARTY.md`.)

To use **your own video** instead, with ffmpeg + libx264 on `PATH`:
`tools\make_clips.bat "C:\path\to\video.mp4"` (any resolution / frame rate) writes both files into `res\` with the settings the built-in clips were made with (H.264, because
HEVC needs a Windows extension that slimmed-down installs lack; ffmpeg itself decodes HEVC sources without it). A clip that is missing stops `make` / `build.bat` before anything is compiled.
Both clips are also unpacked by a running `sentinel.exe` into `%LOCALAPPDATA%\SentinelWatch\content\afk\` (`sentinel_afk_4k60.mp4`, `sentinel_afk_1080p30.mp4`), which is where `build_portable.bat` looks if `res\` has none.

### Build without installing a compiler

`build_portable.bat` builds `sentinel.exe` on a PC that has no C++ compiler. Nothing gets installed: it downloads a portable MinGW-w64
(WinLibs GCC 16.1.0, POSIX threads, SEH, MSVCRT: a 264 MB zip from GitHub) **once**, unpacks it into `C:\SentinelBuild\toolchain`, and compiles there.

1. Clone this repository (or "Code" -> "Download ZIP" on GitHub and unpack it) and double-click `build_portable.bat` in that folder.
2. It needs Windows 10/11 (`curl.exe` and `tar.exe` are part of Windows), an internet connection on the first run, and about 2 GB free on `C:`.
3. The two clips are in `res\` (see "The two clips"); the script checks this **before** it downloads anything.
4. The result is `sentinel.exe` next to the script (`sentinel_built.exe` if a `sentinel.exe` is already there). Sources and compiler stay in
   `C:\SentinelBuild`; delete that folder and nothing is left behind (no installer, registry entry or PATH change).
5. A failed build prints the compiler's messages and saves them to `C:\SentinelBuild\sentinel-watch\build\build.log`: that file is what to send.

Details: a 64-bit `g++` (GCC 10 or newer) that is already on `PATH` is used instead of downloading. The download is checked against the `.sha256`
file published next to it, and an interrupted download resumes. If the download is blocked, fetch the zip in a browser (the script prints the address),
save it into `C:\SentinelBuild` under its own name and run the script again. The work folder is a short plain path on purpose: GCC on Windows
struggles with spaces and accented letters in paths (an account name like "Nguyễn" is common). Environment variables: `SW_BUILD_ROOT=D:\folder`,
`SW_FORCE_PORTABLE=1` (ignore an installed g++), `SW_NOPAUSE=1`. The pinned release is at the top of the script (`TC_TAG`, `TC_ZIP`).

How far this was tested: the script's logic ran under Wine against stand-ins for `curl`, `tar`, `certutil`, `g++` and `windres` (`tests/portable_build/`):
first run, re-run, damaged download, missing clips, missing zip, compiler already installed / 32-bit / too old, project folder with edits,
a work folder with spaces and brackets, failed unpacking, a compile error, no `curl`. What has **not** run is the real thing: the actual download,
Windows' own `tar.exe` on that zip, and compiling with that exact GCC 16.1.0.

## 2. Where things are

```
shared/          sw_common (log, paths, UTF-8 helpers), settings.json load/save, version, small lock/COM helpers, platform.h (which OS this is),
                 autostart.h ("Start with Windows": the Run value and how the autorun watch recognises it; pure logic),
                 eco.h ("Power saving when away": when it starts and stops, how it gets back, the numbers; pure logic over a small interface),
                 update.h ("Automatic updates": version compare, which release is acceptable, SHA-256, the checksum file; pure logic, section 13)
core/            the monitors: autorun_watch, proc_watch (WMI process-creation feed), file_watch (ReadDirectoryChangesW),
                 token_guard, trust (Authenticode), response (block in Windows Firewall), events (the shared feed),
                 str_scan.cpp (reads a flagged program's strings off-thread) + str_scan.h (the pure logic, no Windows headers),
                 eco.cpp (power saving after 30 min without input: a temporary copy of the power plan through powrprof.dll, loaded at run time; marker file),
                 update.cpp (automatic updates: WinHTTP loaded at run time, one worker thread, the swap of the exe)
mac/             the macOS app (section 12): core.cpp + sys.h (portable logic, tested on any host), sys_mac.cpp + apple.mm (libproc, codesign, CoreGraphics,
                 FSEvents), app.mm (AppKit: menu bar, WKWebView dashboard, toast, AVFoundation AFK screen), ui/dashboard.html, content/, Info.plist, mac.mk;
                 sentinel-strings.cpp: the strings check as a macOS command-line tool (same str_scan.h, section 11)
ui/              main (hidden message window, tray, timers), dashboard (GDI+ control panel), toast, gfx, content (quotes + AFK detection)
afk/             the AFK screen
                   afk_screen     start/stop of the controller thread + the supervisor that rescues a hung GPU driver
                   afk_loop       the screen itself: appear / frame loop / leave, focus, display-off and lock handling
                   afk_tiers      back-end lifecycle, the tier ladder, what the app remembers about this PC
                   afk_gpu_backend + afk_overlay_d2d   tiers 0 and 1: GPU player + Direct2D HUD
                   afk_cpu_backend                      tier 2: Media Foundation source reader + GDI+ (last resort)
                   afk_assets     unpacks the built-in clips, finds / probes a clip you dropped in
                   afk_helpers.h  all the pure logic (timing, fades, pacing, geometry, ladder, watchdog, memory) -- no Windows headers
media-engine/    D3D11 + Media Foundation Media Engine player behind the C ABI
                   MediaEngine_Init / Pump / RenderFrame(Ex) / SetPlaybackState / Resize / SetVolume / GetStats / Shutdown
third_party/     mfmediaengine.h (the Media Engine interfaces; MinGW's headers lack them)
res/             sentinel.rc, avatar.jpg, afk_bg_4k60.mp4 and afk_bg_lite.mp4 (the two clips: see "The two clips")
tests/           afk_tests.cpp, str_scan_tests.cpp, autostart_tests.cpp, eco_tests.cpp, update_tests.cpp, mac_logic_tests.cpp, mac_cli_test.sh (native), cpu_decode_harness.cpp,
                 path_unicode_test.cpp, idle_cpu_test.cpp (Wine/Windows), update_e2e.sh + fake_release_server.py (the update end to end under Wine, section 13)
tools/           make_clips.bat (makes the two clips from your video), v13_reference_noblob.cpp (the old single-file version, kept for reference only, not built)
installer/       windows/sentinel.nsi (the Windows setup, NSIS), macos/make_dmg.sh (the Mac disk image)
.github/workflows/build.yml   tests, the Windows exe + setup, the Mac dmg, and the release when a version tag is pushed (section 13)
build_portable.bat   builds without a compiler installed (downloads a portable one); build.bat is the plain build, make is the Makefile build
```

## 3. The AFK screen

### What you see
Video (4K 60 fps with sound, looping) -> fade -> vignette -> starfield (dimmed inside the lens circle, so the focus point
stays clean) -> bottom gradient -> clock -> quote -> hint. Any key / mouse movement ends it. It takes real keyboard focus
when it appears.

### How it plays smoothly
* **GPU first.** Tier 0 plays the 4K 60 fps clip with the Media Foundation *Media Engine* (the same DXVA pipeline browsers use):
  hardware decode -> GPU video processor -> flip-model swap chain (3 buffers, max frame latency 2). The CPU never touches pixels.
  The HUD is drawn with Direct2D on the same back buffer.
* **Preload.** The player is built **before** the screen appears; the first frame is already decoded and the window is hidden, so the
  screen shows up with video instead of loading in front of you. That parked player runs **one** screen: once you have been idle for most
  of the wait (half of it, at most 30 s before the screen is due) the player that already ran a screen is dropped and a fresh one is built
  (it is never resumed), so the next screen finds a ready player again.
* **Video check.** On every screen, once the player is told to play, at least 4 *new* video frames must arrive within 4.5 s; the clip (and its
  sound) is not shown before that. If they do not, the log says `VIDEO CHECK FAILED` with the player's own state, the player is rebuilt once,
  and a second failure goes one tier down. A screen that ends without a moving picture says `NO VIDEO was shown` in its `screen down` line. While **power saving** (section 4) is
  on, the CPU is capped, so neither this check nor the watchdog judges the player (`video check skipped: power saving is on` in the log); nothing is remembered against a tier then.
* **Tier ladder (automatic).** 0 = 4K 60 fps on the GPU -> 1 = 1080p 30 fps on the GPU -> 2 = CPU renderer (software
  decode, GDI+). A watchdog counts the video frames that are really delivered (3 s warm-up, then 2 s windows; below 60 % of the
  clip's frame rate twice in a row = too slow) and swaps the tier **while the screen stays up**.
* **What the app remembers.** A slow verdict becomes permanent only after two sessions in a row (a 30 s run without trouble
  clears the count); a hard failure (the GPU player cannot start, repeated device loss) is remembered at once; every verdict
  expires after 7 days, and is dropped when the build or the `afkGpu` choice changes. Only the tier a player *started* on is judged.
  A crash / hang / kill while a tier is active leaves a marker file (`afk_active.flag`); the next start of the same build
  treats that tier as failed.
* **Supervisor.** GPU drivers sometimes block a call for good. The UI thread watches the controller's heartbeat (45 s, 180 s while
  unpacking the clips); a stuck controller is abandoned, the GPU player is switched off for the session and a CPU-only
  controller takes over.
* **Power rules.** AFK screen: one frame per display refresh while visible; display off, locked screen or covered window =
  nothing rendered, playback paused after 3 s. Dashboard: 30 fps open, 5 fps minimized, no drawing at all while the AFK screen
  is up. AFK detection is a cheap timer on the hidden message window.

### Your own video
Drop any `.mp4 .m4v .mov .mkv .webm .avi .wmv` into `%LOCALAPPDATA%\SentinelWatch\content\afk`. It outranks the built-in clips
and is used on every tier (it has no lighter copy, so pick one your PC decodes). Delete it and the built-ins come back.

The built-in clips are **H.264**, not HEVC: Windows only decodes HEVC with the separate "HEVC Video Extensions", which
slimmed-down installs (AtlasOS and similar) normally do not have. A clip of yours that is HEVC works only where that
extension is installed; the log says so.

## 4. Settings: `%LOCALAPPDATA%\SentinelWatch\settings.json`

Edit it while the app is closed (the app rewrites the file whenever a toggle or its own memory changes).

| Key | Values | Meaning |
|---|---|---|
| `afkMode` | `"auto"` (default), `"gpu"`, `"cpu"` | `auto` walks the ladder; `gpu` never uses the CPU renderer; `cpu` forces the GDI+ renderer |
| `afkIdleSec` | `0` (default), `5`..`3600` | `0` = after the Windows screen-saver wait if one is set, else 5 min. **`10` is handy for testing** |
| `afkGpu` | `"default"`, `"intel"`, `"nvidia"`, `"amd"` | which graphics chip plays the video on a PC with two. `default` lets Windows choose |
| `afkMinTier` | `0`..`2` | the app's own memory (0 = try everything). Set to `0` to force a fresh try of every tier |
| `afkStrikes`, `afkMinTierAt`, `afkBuild` | | bookkeeping behind the memory; leave them alone |
| `autorun proc file allfiles tokens strings notify beep ignoresys autostart eco update` | `true` / `false` | the toggles shown on the dashboard (`strings` = the strings scan, section 10; `autostart` = start with Windows and `eco` = power saving when away, both below; `update` = automatic updates, section 13, the "AUTO-UPDATE" switch on the Updates tab; `strings`, `autostart`, `eco` and `update` are on by default) |
| `splashMs`, `dashW`, `dashH` | numbers | splash time and dashboard size |

The same text is written to `content\afk\README.txt` on every start.

**Start with Windows** (`autostart`, on by default, the last switch on the Protection tab). While it is on, the app keeps one value in
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run` (no admin rights, this user only): `SentinelWatch` = `"<full path of sentinel.exe>" --background`. A start with `--background`
shows no splash and no dashboard: the app sits in the tray (double-click the icon, or start the exe again, to open it). Turning the switch off deletes the value; moving the exe
only needs one run from the new place (the value is rewritten with the new path). The autorun watch knows this one entry (same name **and** same path) and does not report it;
the same name pointing at another file is still reported. The log says `[AUTOSTART] on: ...` / `off: ...`, and the first line of a hidden start ends with `(hidden in the tray)`.

**Power saving when away** (`eco`, on by default, the last switch on the Protection tab). After **30 minutes without keyboard or mouse input** the PC is put on a power-saving
setup, and the first input puts everything back:

* **CPU capped at 25 %** ("Maximum processor state", plugged in and on battery; a lower value you already use stays), the minimum set to at most 5 %, **boost off**, and **idle states
  allowed** (some tuned Windows plans switch them off; an idle CPU saves its power there)
* **screen dimmed** to at most 15 % (a dimmer setting stays as it is)
* how: the app makes a **temporary copy of the power plan you are on** ("Sentinel Watch power saving (temporary)"), changes those values in the *copy*, and makes the copy the active plan.
  Going back activates **your own plan** again and deletes the copy, so your plans are never edited: the brightness and everything else come back as your plan has them.
  Your sleep / display-off timers are not touched either (they are copied unchanged). It is not Windows' own "Power saver" plan.
* undone: at the first input (within a second), when the app closes, when Windows signs out / shuts down / restarts (a hidden window hears it), and, if the app was killed or crashed
  while it was on, **at the next start** (a marker file `eco_active.txt` in the data folder names your plan and the copy; deleting it while the copy is active leaves the copy in place:
  pick your plan in the Windows power options). If your plan is gone by then, Windows' Balanced plan is used.
* without a marker nothing is changed (if the marker cannot be written, power saving does not start); a failed attempt is retried after a minute, three failures in a row stop it until you have been
  back once. The log says why: `[ECO] power saving ON ...`, `OFF: back on plan ...`, `NOT started: ... error N` (error 5 = access denied: run the app as administrator once to see if that is it).
* the numbers are `ECO_AFTER_MS`, `ECO_CPU_MAX_PCT` and `ECO_BRIGHT_PCT` in `shared/eco.h` (and the text of the switch in `shared/settings.cpp`).
* it keys on **input only**: a video, a game, a download or a render that runs while you do not touch the PC counts as "away" too. The AFK video itself keeps playing during power saving
  (it is the biggest consumer left; say if it should pause or go black there).

On a laptop with Intel + NVIDIA (Optimus) Windows normally puts a windowed app on the GPU that drives the screen (usually the
Intel one). To make it use the NVIDIA chip: *Settings -> System -> Display -> Graphics -> add `sentinel.exe` -> Options -> High
performance*, or set `"afkGpu": "nvidia"`.

## 5. The log: `%LOCALAPPDATA%\SentinelWatch\events.log`

Everything the AFK screen does is logged, with HRESULTs. Tags: `[AFK]` decisions, `[AFKVID]` video, `[AFKVID] stats` every 5 s while the screen is up,
`[AFK-GPU] [ME]` the GPU player's own lines (its events, the result of every first `Play` / `Pause`, why a first frame took long), `[STR]` the strings scan of a flagged
program (section 10), `[AUTOSTART]` the start-with-Windows switch, `[ECO]` power saving when away (section 4) and `[UPD]` the automatic update (section 13).

A healthy 4K start looks like:

```
[AFK] built-in 4K clip: ...\sentinel_afk_4k60.mp4 (3840x2160 @ 60.0 fps, H264, 32.0 s)
[AFK] preloading: tier 0 (...), mode auto, remembered minimum tier 0
[AFK] tier 0 started: ...            [AFK] tier 0 is ready: first video frame NNN ms after start
[AFK] screen up: 3840x2160 at 60 Hz ... tier 0
[AFK] video check passed: 4 new frames within 0.2 s of the player being told to play; the clip is revealed (tier 0)
[AFKVID] stats (GPU, 3840x2160): video 60.0 fps of 60.0 | display 60.0 fps (sync 1) | per frame: frame-transfer ... | longest gap between video frames ... stalls over 120 ms 0 | engine dropped 0 | <your GPU>
```

The last line of a screen is `screen down after N s (...): tier T, N video frames, ..., video check passed 0.3 s in`. **No video on a screen** shows up there as
`NO VIDEO was shown` (the player was still loading) or `NO MOVING VIDEO was shown` (it loaded but never played), and the lines just above it say why:
`screen up ... still loading`, `VIDEO CHECK FAILED ... -- player: <state>` (the engine's own state: paused? clip time? result of the last `Play()`?),
`[ME] Play (...) -> hr=0x...`, or `the screen is due but cannot come up: <reason>` when it was never shown at all.

If it is not smooth, the lines that matter are `slow window`, `tier N failed`, `stalls over 120 ms`, `engine dropped`, and the adapter name at the
end of the stats line (is it the GPU you expect?). If the video never appears, look for `[ME] ... no first frame ... (events: metadata=.. data=..)` (which
events the player saw), `source attempt N ... shows no sign of life` and `media engine error`. Send those.

**CPU use.** Once a minute in which the app averaged 3 % or more of the whole CPU (the Task Manager scale), one line names the busiest threads:

```
[CPU] last 60 s: app 28.4 % of the CPU (4 logical) | tokenguard 24.9 | ui 2.1 | tid 4412 0.9
```

Thread names: `ui` (message loop and the dashboard repaint), `afk` (the AFK screen), `tokenguard`, `wmi` (process watch), `dir` (AppData file watch),
`scan` (the 5-minute scan), `update` (the update check), `cpu-video` / `cpu-audio` (CPU renderer); `tid N` is a thread the app did not name (Media Foundation, GDI+, driver).
No `[CPU]` line = the app stayed under 3 %. With the AFK screen down the app should be close to idle; if a line appears then, it says what to look at.
The dashboard repaints at once when something changes and a few times a second otherwise (once a second while another window has the focus);
the signature check of a running app is done once per file version and then remembered.

## 6. The design document vs. this build

| Design document | This build |
|---|---|
| C++ D3D11 media engine with the C ABI `MediaEngine_Init / RenderFrame / SetPlaybackState / Shutdown` | **Done** (`media-engine/`, also buildable as a DLL) |
| Direct2D HUD | **Done** (`afk_overlay_d2d.cpp`); the CPU tier keeps a GDI+ copy of the same look |
| Power rules (60 fps focused, 5 fps minimized, paused hidden) | **Done** (section 3) |
| External HEVC MP4 | Replaced by two **embedded H.264** clips; an external clip in `content\afk` still works |
| Rust daemon with the monitors | **Not ported.** The monitors stay in C++ in the same exe (`core/`) |
| Named pipe `\\.\pipe\sentinel_watch_pipe` | **Not implemented** (there is no daemon to talk to; the UI and the AFK thread share state in-process) |

## 7. What was verified, and what was not

Built and checked on Linux with the mingw-w64 GCC 13 cross compiler (win32 thread model) and Wine 9.0. **Nothing here has run on real Windows or a
real GPU.**

Verified:
* both exes build with **0 warnings** (`-Wall -Wextra`); all sources also compile clean as C++20 / C++23; a second compiler (Zig 0.17's clang with libc++
  and newer mingw-w64 headers) accepts every file too (its only remarks are the style warning the Makefile switches off)
* **62,579 native unit checks, 0 failed**, also under AddressSanitizer + UBSan and under ThreadSanitizer (no reports): timing, fades, pacing,
  geometry, frame-layout / render-grid logic, tier ladder, watchdog, tier memory, GPU preference, media-engine helpers, the video check (pass / fail / deadline / counter
  reset) with the reveal hold, and the preload lead
* **113 checks** for the strings engine (links, `ip:port`, hot hosts, Windows and macOS commands, UTF-16, runs and limits, Mach-O sniffing; also under ASan + UBSan)
  and **21 checks** for the Mac tool on a tree of fake programs (folder search, Mach-O / script detection, links not followed, exit codes, files over 16 MB)
* the strings scan end to end under Wine: fake droppers copied into the watched folder give the `[STR]` lines, hot links first, noise links left out, UTF-16 text,
  the link in the tail of a 17.8 MB file (over the 16 MB limit), a clean file, the same file renamed back (not read twice), option off = no `[STR]` lines; a 16 MB scan takes about 0.2 s natively
* **13 checks** for the start-with-Windows rule (the `Run` value, recognising our own entry and not an imitation of it) and **304 checks** for the Mac app's portable core
  (`mac/core.cpp`: text and JSON safety, trust rules, signature checks, settings, event feed, process / login-item / token-file / new-file watchers, strings scan,
  LaunchAgent, quotes, the dashboard's JSON, the engine loop) against a fake operating system. That core is plain C++17 and was built and run on Linux
* the Mac tool built for Apple Silicon: Mach-O arm64, macOS 11 or later, links only `libSystem`, ad-hoc code signature present (header checks; it was **not run**)
* **43 checks** for the rules of power saving (`shared/eco.h` over a fake power API: when it starts and stops, a failed switch and the retry limit, going back when your plan is busy or gone, the marker,
  recovery after a crash; also under ASan + UBSan). Under Wine, with in-memory fake plans: ON after the idle time (`SW_TEST_ECO`), OFF within about a second of real input (X11 pointer events), ON again,
  a killed run leaves the marker and the next start names it and clears it, a sign-out message (`WM_ENDSESSION`) puts the plan back before the process is gone, and an AFK screen that begins while power saving
  is on skips the video check and the watchdog and stays on tier 0. The 11 switches fit the Protection tab at 780x560 and 940x600 (checked by geometry: this Wine draws no text)
* Windows start-up under Wine: the `Run` value is written (`"<exe>" --background`), a start with `--background` shows no splash and no dashboard and logs `(hidden in the tray)`,
  the switch off removes the value, a moved exe rewrites the path, and the autorun watch does not report the app's own entry (but does report the same name pointing elsewhere)
* **81 checks, 15 scenarios** drive the real CPU decode worker through a scripted fake reader: bad / short / bottom-up / 2-D buffers, read errors,
  failing loop restarts, format changes mid-stream, a clip that vanishes. The same harness fails 30 checks on the code before the review fixes
* **84 checks** that the data folder, log and settings work under accented, Vietnamese, Japanese and emoji account names (the old narrow-path code fails them)
* Wine smoke runs of the real exe: CPU-tier enter / leave cycle, the GPU ladder (soft strike, watchdog downgrade), a hung GPU driver
  (supervisor takes over after the 2 s confirmation), the crash marker (this build = failure verdict, another build = ignored), real idle detection
* several AFK screens in a row (3+ per Wine run, also with the dashboard minimized or closed, and through the real idle path with `"afkIdleSec": 20`): the player is renewed about half a wait
  **before** the next screen, so every later screen starts on a ready player and logs `video check passed` (no `still loading`); `[ME] init` / `shutdown` balanced; slow verdicts counted per player.
  `SW_TEST_STUCK=<n>` (the first n players ignore the request to play): `VIDEO CHECK FAILED` after 4.5 s with the engine's state, a rebuilt player, a second failure goes a tier down, and with no
  lower tier `there is no lower tier` and `NO MOVING VIDEO was shown`. **Not proven:** that this cures the "video sometimes missing" report on your laptop. That cause was never reproduced: what was
  found is a flaw in the logs of the repeat runs (the player was replaced only when the screen was already due, so every later screen started on a player that was still loading); the video check and the
  log lines are the net under anything else
* **107 checks** for the updater's rules (`shared/update.h`: version compare, SHA-256 against the standard test vectors, the JSON reader, which releases are accepted and which refused, the checksum file,
  the "installed before" guard, https-only addresses; also under ASan + UBSan). Under Wine, `tests/update_e2e.sh` runs the real exe against a fake release server on the same PC: a newer release is
  downloaded, checked, swapped in and the new copy starts (exactly one restart, `.old` removed, `update_applied.txt` written); a wrong checksum, a cut-off download, the same version, a pre-release, a missing
  release (404) and the switch off each leave the exe untouched; with the dashboard open the update is downloaded but not installed
* the Windows setup, run silently under Wine (`tests/installer_test.sh`, built around a stand-in exe): it closes a running copy, replaces the program file, removes the leftovers of an update, writes the start menu
  link and the uninstall entry; the uninstaller removes all of it plus the "start with Windows" value and keeps the data folder (a silent uninstall never asks)
* the static import list is the original one plus GDI32 and WINMM (the updater adds none: WinHTTP is loaded at run time)
* three independent code reviews (CPU back-end, controller, GPU player); every confirmed finding is fixed, apart from the blind-mode weakness, which is documented below
* the Wine runs also exercise the real Media Engine code path as far as Wine goes (it loads the clip, sends the events, captures the first frame; it never signals
  new frames, so blind mode and the watchdog downgrade are what gets tested)

**Not verified -- test on your PC:**
* real GPU decode and present of the 4K 60 fps clip on the GTX 1050 / HD 630 (is it smooth, which adapter is chosen, how long the first frame takes), and whether **4 new frames within 4.5 s** is the
  right bar for the video check there (a false `VIDEO CHECK FAILED` costs one player rebuild, then maybe a tier; the log line says what the engine was doing)
* the Direct2D / DirectWrite HUD (Wine cannot draw it); audio (the media-session loop and teardown were only desk-checked); the seam at the loop point
* display-off and lock-screen behaviour; which adapter gets picked (Intel or NVIDIA); whether the H.264 4K 60 fps decode works on both chips
* a build with GCC 16.1 posix-seh (only GCC 13 win32 and clang 18 were available here)
* the strings scan on real Windows and on real droppers (the samples were made up: a fake exe with a webhook, links and commands in it), and how often honest unsigned
  programs trigger it
* **power saving on real Windows**: the real `powrprof.dll` calls (Wine has none: there the log says `powrprof.dll does not offer the power-plan functions`); whether a standard account may copy and
  switch power plans (an access error is logged, not hidden); that the screen really dims, the CPU really stays at 25 % (Task Manager -> Performance -> CPU -> Speed) and that your own brightness and plan come
  back; what the copied values do on AtlasOS's power plan; how long the calls take (they run on the UI thread)
* **the update against real GitHub**: the real HTTPS / TLS call of WinHTTP to `api.github.com` and the redirect from `github.com/.../releases/download/` to GitHub's download servers (the fake server speaks plain http, and only
  test builds allow that); the swap with a real antivirus or SmartScreen looking at the new file; a rollback after a failed swap (the code is there, no test makes it fail on purpose); the setup's pages and its "delete the data folder?" question (nobody can click them here) and its use
  on real Windows; the GitHub workflow (`.github/workflows/build.yml`: it was only parsed, GitHub runs it for the first time on the first push); the Mac dmg
* the Mac tool on a Mac: no macOS was available, so the Apple Silicon build was never run (Gatekeeper's reaction, `std::filesystem` on macOS and the first-run steps in section 11 are unchecked)
* **the Mac app (section 12) has never been compiled**: `apple.mm`, `app.mm` and `sys_mac.cpp` (AppKit, WKWebView, AVFoundation, CoreGraphics, FSEvents, libproc, `codesign`) are untested, and so are Gatekeeper,
  the Downloads permission prompt and the LaunchAgent on a real Mac
* what does **not** exist in this environment: HEVC / H.264 decoding under Wine (the CPU tier ends in "no video" there), so CPU-tier *decoding* is covered by the
  harness only, not by a real clip

## 8. Known limitations

* A GPU driver that hangs **while the AFK screen is showing** can leave that window frozen on top until the app is closed
  (tray -> Exit, or Task Manager). The supervisor still switches the app to the CPU renderer for the next time.
* If Windows refuses to hand over keyboard focus, the first key press may also reach the window underneath (logged).
* "Blind mode" (the fallback when the Media Engine never reports new frames: frames are then counted from the media clock) cannot tell a starved decoder
  from a healthy one, so the watchdog is weaker there. The stats line ends with `| blind mode` when it is active. Under Wine it always is.
* One GPU back-end at a time; no waitable swap chain. A lost GPU device is rebuilt in place (up to 3 times within a minute; the 4th drops a tier).
* A clip you drop in has no 1080p 30 fps copy: if tier 0 is too slow for it, the next step is the CPU renderer, not the lite GPU tier.
* Strings scan: see the limits at the end of section 10.
* Power saving (section 4) looks at input only, makes its power-plan calls on the UI thread (if the Windows power service stalls, so do the tray and the dashboard; the AFK screen has its own thread),
  leaves the AFK video running, and does not judge the AFK player while it is on (a screen that fails only under the cap goes unnoticed).
* Automatic updates (section 13): Windows only (the Mac app does not update itself); the exes are not code-signed, so what protects an update is HTTPS, the GitHub account that owns the repository and the checksum in the release;
  the repository must be public; the app must be able to write next to its own exe (an install under `C:\Program Files` would need administrator rights: the installer uses `%LOCALAPPDATA%\Programs` for that reason).

## 9. Test hooks (`sentinel_test.exe` only)

`SW_TEST_AT=<s>` pretend idle after s seconds - `SW_TEST_FOR=<s>` ... and back s seconds later - `SW_TEST_MODE=auto|gpu|cpu` - `SW_TEST_HANG=1`
block in the first GPU start - `SW_TEST_NOINPUT=1` ignore real input - `SW_TEST_STUCK_MS=<ms>` supervisor limit -
`SW_TEST_EVERY=<s>` repeat the idle period every s seconds (several AFK screens in one run) - `SW_TEST_DASH=min|close` start with the dashboard minimized / closed to the tray -
`SW_TEST_STUCK=<n>` the first n GPU players ignore the request to play (exercises the video check) -
`SW_TEST_ECO=<s>` power saving after s seconds without input, or `SW_TEST_ECO=<at>,<for>` "away" from second `at` for `for` seconds whatever the input does (a window that appears counts as input under Wine);
the power plans are in-memory fakes, `SW_TEST_ECO_REAL=1` makes the real calls -
`SW_TEST_UPDATE_URL=<http://host:port/path>` ask this address instead of GitHub's (plain http is allowed only in the test exe; the asset addresses in the answer must start with the same `http://host:port/`) -
`SW_TEST_UPDATE_FIRST=<ms>` first look after this long instead of 90 s - `SW_TEST_UPDATE_IDLE=<s>` how long the PC must be idle before an update is installed (default 60; `0` for tests).
Under Wine note: a `LOCALAPPDATA` given from the shell is ignored by Wine, `LANG=C.UTF-8` is needed for non-ASCII file names, and Wine's D3D11 is a slow
fake adapter.

## 10. The strings scan (links and commands inside a flagged program)

When Sentinel flags a program -- a new unsigned executable in a user folder, a new unsigned process, the 5-minute scan, a token-file access -- it also reads
the file's strings (like `strings -a`: ASCII and UTF-16 text of 6 or more characters) and reports what malware tends to carry:

* **Links**: http / https / ftp / ws / wss addresses and `ip:port` pairs. Webhooks, bot APIs, paste and file-sharing sites, tunnels and URL shorteners are marked
  `[!]` and listed first (where a dropper downloads from, where a stealer sends what it took). XML namespaces, certificate services and loop-back addresses are ignored.
* **Commands**: command lines malware is known for: encoded / hidden PowerShell, `certutil -urlcache`, `bitsadmin /transfer`, `mshta http`, `schtasks /create`,
  shadow-copy deletion, Defender exclusions, `reg add ...\Run`, and the macOS ones (section 11).

It is **on by default**: the Protection tab has a *Strings scan* switch, and `settings.json` has `"strings": true` (`false` turns it off). The result is an `STR` row
in the events list, a toast ("Links / commands inside it") and lines in `events.log`:

```
STR strings: 2 link(s), 1 command(s) | C:\Users\you\AppData\Roaming\x.exe
[STR]   [!] https://discord.com/api/webhooks/123/abc
[STR]   http://203.0.113.9:8080/p.bin
[STR]   cmd: powershell -nop -w hidden -enc JABz...
[STR] C:\Users\you\AppData\Roaming\y.exe: no links or suspicious commands in its strings
```

* A hit is a **lead, not a verdict**: honest unsigned programs carry links too (updaters, telemetry, help pages), and malware can pack or encrypt its strings, so
  "nothing found" proves nothing. An unsigned program with a few links will now also raise one more toast.
* It only reads the file: nothing is run, nothing is fetched, no network. One low-priority thread works through a queue (32 files at most; more in a burst are dropped),
  waits until a new file has stopped growing (5 s at most) and reads at most 16 MB per file: the first and the last 8 MB of a bigger one, so what lies between is not scanned.
  A file is not read again until its size or time changes (the memory is lost at restart). Sentinel's own exe and data folder are skipped.
* Not looked for: API-function names, credential-file names, packed or encrypted text, anything that is not text in the file.

## 11. macOS (Apple Silicon) command-line version

`mac/sentinel-strings.cpp` is the same strings check as a small command-line tool for macOS (arm64 or Intel). It uses the same engine (`core/str_scan.h`), which also
knows the macOS tricks: `osascript` password prompts (`with hidden answer`, `with administrator privileges`, `do shell script`), `curl ... | bash`,
`bash -c "$(curl ...)"`, `base64 -D | bash`, `security find-generic-password` / `dump-keychain`, `xattr` quarantine removal, `spctl --master-disable`, `TCC.db` edits,
`launchctl load`. Apple's own certificate and Info.plist links are ignored.

```sh
sentinel-strings ~/Downloads/Thing.app   # a folder: every Mach-O program and #! script in it (symbolic links are not followed)
sentinel-strings ./some-file             # a file you name is always checked
```

It prints the same lines as the Windows log (section 10) and ends with `checked N file(s), M with links or suspicious commands`. Exit code 0 = nothing found,
1 = something found, 2 = a path could not be read. It only reads: nothing is run or fetched.

Get it either way:
* **Build it on your Mac** (Xcode command line tools: `xcode-select --install`): `clang++ -std=c++17 -O2 -Icore mac/sentinel-strings.cpp -o sentinel-strings`
* **Use the prebuilt `sentinel-strings-macos-arm64`** (cross-built on Linux with Zig; Mach-O arm64, macOS 11 or later, ad-hoc signed): `chmod +x sentinel-strings-macos-arm64`, and if macOS
  refuses to open it, `xattr -d com.apple.quarantine sentinel-strings-macos-arm64` removes the mark a downloaded file carries. `make macarm` rebuilds it.
  That prebuilt file comes from the Nov 3 package: it was built before the capped file read was split out into `mac/scanfile.h` (shared with the Mac app, Nov 4). `tests/mac_cli_test.sh` passes on the current
  source, but the prebuilt was **not** rebuilt from it (no Zig in the last session): if you want the exact current source, build it on the Mac (above) or with `make macarm`.

It is **not** the whole Sentinel: there is no dashboard, no AFK screen, no folder watching and no signature check (it checks whatever you give it). The whole suite for the Mac is the app in section 12.


---------------------------------------------------------------------------------------------------------------------

## 12. macOS app (menu bar, dashboard, AFK screen)

**Never compiled and never run.** It was written on Linux without a Mac. What was tested is the part that has no Apple code in it (`mac/core.cpp`, 304 checks against a fake operating system, also
under ASan + UBSan). `apple.mm`, `app.mm` and `sys_mac.cpp` (AppKit, WKWebView, AVFoundation, CoreGraphics, FSEvents, libproc, `codesign`) have only been read: expect compiler errors on the
first build, and send them back verbatim.

**Files** (`mac/`)

| File | What it is |
|---|---|
| `core.cpp` / `core.h` | the portable part: settings, event feed, trust rules, the four watchers, the strings scan (`core/str_scan.h`), LaunchAgent text, quotes, the state the dashboard reads as JSON |
| `sys.h`, `sys_mac.cpp` | the few operating-system calls the core needs (processes through libproc, `codesign --verify --strict -R "=anchor apple generic"` through `posix_spawn`); the tests use a fake `sw::Sys` |
| `apple.h`, `apple.mm` | idle time (`CGEventSourceSecondsSinceLastEventType`), screen lock (`CGSessionCopyCurrentDictionary`), `~/Library` plists (Foundation), folder events (FSEvents) |
| `app.mm` | the app: menu-bar item, dashboard window (a WKWebView), toast, AFK screen (AVFoundation) |
| `ui/dashboard.html`, `content/*.txt`, `Info.plist`, `mac.mk` | dashboard page (tabs Dashboard / Protection / Events / Updates), quotes and facts (`extract_content.pl` made them from `ui/content.cpp`), app description, build rules |

**Build** (on a Mac with the Xcode command line tools: `xcode-select --install`; no Xcode, Homebrew or Zig needed)

```sh
./build.sh          # or: make        -> build-mac/SentinelWatch.app   (arm64 on Apple Silicon, x86_64 on an Intel Mac; ad-hoc signed; macOS 11 or later)
make ARCH=x86_64    # the other architecture
make run            # build, then open the app
make tests          # the unit tests: 304 checks of the Mac core + the strings engine + the command-line tool + autostart
```

`./build.sh` and `make` ask the operating system first (`uname -s`, `shared/platform.h` for the sources): Darwin builds this app, Windows (MinGW) builds `sentinel.exe`, Linux cross-compiles the
Windows exe (`CROSS=x86_64-w64-mingw32-`); anything else stops with a message. Put the two AFK clips in `res/` before building (section 1, "The two clips") or the AFK screen has no video.

**What it does, next to the Windows version**

| | Windows | macOS app |
|---|---|---|
| Where it lives | tray icon, dashboard window | menu-bar item (no Dock icon): Open dashboard, Quick scan, Notifications, Quit Sentinel Watch |
| Watchers | autoruns (`Run` / `RunOnce` keys, Startup folders, Task Scheduler), processes, files, Token Guard | LaunchAgents / LaunchDaemons, processes (every 2 s), files in `~/Library/Application Support`, `~/Library/LaunchAgents`, `~/Downloads`, `/private/tmp`, `/private/var/tmp`, `/Users/Shared`, Token Guard |
| "Is it signed?" | WinVerifyTrust | `codesign --verify --strict -R "=anchor apple generic"`: the signature is valid AND its certificate chain leads to Apple (Developer ID, App Store, Apple's own). An ad-hoc or self-signed program counts as unsigned |
| Strings scan | on by default | on by default, same engine |
| Start with the computer | `Run` value, hidden in the tray | LaunchAgent `com.sentinelwatch.agent.plist` with `--background` (menu bar only), switch "Start at login" |
| AFK screen | GPU player (4K 60), tiers, video check, watchdog | `AVQueuePlayer` + `AVPlayerLooper` on every display, clock and quote on the main one; a clip of your own in `<data>/content/afk` wins over the built-in ones |
| Power saving after 30 min away | yes (section 4) | **no** |

Switches (Protection tab; the same names as in `settings.json`): `autorun proc file allfiles tokens strings notify beep ignoresys autostart`. `"afkIdleSec"` works as on Windows
(0 = 5 minutes, 5..3600 = that many seconds; 10 is handy for testing).

**Where its files are:** `~/Library/Application Support/SentinelWatch` (`settings.json`, `events.log`, baseline, the clip folder `content/afk`); `$SW_DATA_DIR` moves it.

**First run**
* A downloaded app is quarantined: if macOS refuses to open it, `xattr -dr com.apple.quarantine build-mac/SentinelWatch.app` (or right-click -> Open). An app you built yourself is not.
* The first look into `~/Downloads` brings macOS's own permission prompt (the only protected folder it watches); answering "Don't Allow" leaves that folder unwatched, nothing else breaks.
* Check that it works: open `~/Library/Application Support/SentinelWatch/events.log`. Wanted: `[SIGN] the signature check works (...)` (a start-up self-test on an Apple program, a plain file and an
  ad-hoc signed copy; if `codesign` answers otherwise the log says `the signature check does not work here ... unsigned-program warnings are off`, loudly, instead of guessing), `[AUTOSTART] on: ...` (once), and with `"afkIdleSec": 10` and your hands off the keyboard
  `[AFK] screen up on N display(s); video: afk_bg_4k60.mp4`, `[AFKVID] 2 s in: looper status 1 (1 ready, 2 failed), player 2 (2 playing)`, then `[AFK] screen down` when you move.
  Status 2 or a player that is not playing is the line to send back (it carries the system's own error text).

**Not there, on purpose:** the GPU tiers, the video check and the watchdog (AVFoundation does the playing; the log line above is the only evidence), power saving, Endpoint Security (needs an Apple
entitlement), a signed / notarised build (this one is ad-hoc signed: it runs on the Mac that built it).

---------------------------------------------------------------------------------------------------------------------

## 13. Automatic updates (Windows)

While the program runs it asks this repository's GitHub releases whether a newer version exists, downloads it, checks it, and swaps its own exe for it **when you are away from the PC**. No prompt.
Switch: `update` in `settings.json` (on by default; the "AUTO-UPDATE" chip on the Updates tab). The Updates tab also shows the status ("Up to date (v1.0), checked 14:03", "Downloading v1.0.1... 42 %",
"v1.0.1 downloaded: installs when you are away from the PC") and has a **Check now** button.

**How it works** (`shared/update.h` = the rules, `core/update.cpp` = the Windows side)
1. 90 seconds after the start, then every 12 hours (30 minutes after a failed check), a worker thread (below normal priority, asleep the rest of the time) asks
   `https://api.github.com/repos/lIIusionator/Sentinel/releases/latest`.
2. The release is used only if it is **not a draft and not a pre-release**, its tag is a plain version (`v1.0.1`, `1.2`) **newer than the running one**, and it has the assets `SentinelWatch.exe` and
   `SHA256SUMS.txt` whose addresses start with `https://github.com/lIIusionator/Sentinel/releases/download/`.
3. It downloads `SHA256SUMS.txt`, then `SentinelWatch.exe` to `<exe>.new` (the SHA-256 is computed while it downloads; at most 400 MiB). The hash must equal the one in `SHA256SUMS.txt`,
   otherwise `.new` is deleted and the log says so.
4. A downloaded update is installed only when **nothing is in the way**: no AFK screen, the dashboard hidden (the program sits in the tray) and **no keyboard or mouse input for 60 seconds**.
   Then the running exe is renamed to `<exe>.old`, `.new` becomes the exe, the program starts the new exe with `--background --after-update`, writes `update_applied.txt` and exits.
   Any step that fails puts everything back and the log says which.
5. The new copy waits (up to 20 s) until the old one has let go of the single-instance mutex, deletes `<exe>.old` / `<exe>.new`, and carries on in the tray.

If a release's exe still said an older version than its tag, every check would download it again. `update_applied.txt` stops that (`[UPD] ... was installed before, but this program still calls itself ...: not downloading it again`),
and the release workflow refuses a tag that is not `SW_VERSION`.

**Security model.** The exes are **not code-signed**, so what protects an update is HTTPS to GitHub, the GitHub account that owns the repository, and the checksum in the release. The checksum catches a damaged or cut-off
download; it does **not** protect against someone who controls the release itself (they could replace both files). **Whoever can publish a release here can run code on every PC that has the program: keep 2FA on
the account and publish releases yourself.** Nothing is fetched from anywhere but `api.github.com` and `github.com/<repo>/releases/download/` (GitHub then redirects the download to its own servers). The request is a plain GET without
cookies or an account; GitHub sees the IP address and `User-Agent: SentinelWatch/<version>`.

**The repository must be public.** The check is an unauthenticated call to GitHub's API: for a private repository GitHub answers "404 Not Found" (the log says `no published release`) and nothing is ever updated.

**Files.** `<exe>.new` (the download, while it waits), `<exe>.old` (only between the swap and the next start), `%LOCALAPPDATA%\SentinelWatch\update_applied.txt`. The program must be able to write next to its own exe: the setup
installs to `%LOCALAPPDATA%\Programs\SentinelWatch` for that reason. The file watcher ignores any path that contains `\sentinelwatch\`, so the swap in that folder raises no alert; an exe kept in a watched folder (Downloads, say)
may be reported once as a new unsigned program after an update.

**Log.** `[UPD]` lines: `up to date`, `<tag> is available ...: downloading`, `<tag> downloaded, SHA-256 matches`, `installed <tag> ...: restarting`, `check failed: <why>`, `cannot install <tag>: <why>`.

**Publishing a release** (whoever maintains the repository)
1. Change `SW_VERSION` in `shared/version.h` (`"v1.0.1"`), and `SW_BUILD_ID` if the AFK behaviour changed (rule 6 in `CLAUDE.md`). Commit and push to `main`.
2. Wait until the push is green (Actions tab), then tag it: `git tag v1.0.1 && git push origin v1.0.1`. The workflow (`.github/workflows/build.yml`) checks that the tag is `SW_VERSION`, runs the tests, builds
   `SentinelWatch.exe` and `SentinelWatch-Setup.exe`, writes `SHA256SUMS.txt` (of both) and publishes the release with the three files; the Mac job adds `SentinelWatch-macOS.dmg` if it works.
3. Installed copies find it within 12 hours and install it when their PC is idle. A draft or pre-release is ignored by them: use that to try the workflow without updating anybody.
4. Without GitHub Actions: build (`./build.sh`, then `makensis -DEXE=<SentinelWatch.exe> -DVERSION=1.0.1 -DOUT=SentinelWatch-Setup.exe installer/windows/sentinel.nsi`), run
   `sha256sum SentinelWatch.exe SentinelWatch-Setup.exe > SHA256SUMS.txt`, create the release by hand (Releases -> Draft a new release, tag `v1.0.1`) and drag the three files in. Publish last: the files must all be there.

**Tests.** `make tests` runs `tests/update_tests.cpp` (107 checks). The end-to-end run under Wine needs two test exes that differ only in their version:
```sh
make CROSS=x86_64-w64-mingw32- TEST=1                                   # sentinel_test.exe, says v1.0
CXXFLAGS='-O2 -DSW_VERSION_OVERRIDE=\"v1.0.1\"' make CROSS=x86_64-w64-mingw32- TEST=1 BUILD=build-test-b OUT=sentinel_test_b.exe
tests/update_e2e.sh sentinel_test.exe sentinel_test_b.exe               # Wine, Xvfb and python3
```
It does not cover real HTTPS to GitHub (see section 7).
