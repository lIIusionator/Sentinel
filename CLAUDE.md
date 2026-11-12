# CLAUDE.md -- Sentinel Watch Suite

Native Windows C++17 monitor + AFK screensaver (looping 4K60 video with sound). One static exe, both clips embedded.
README.md has the user-facing description; this file is for whoever (human or AI) changes the code next.

## The user and what they asked for (keep honoring it)
* Windows, MinGW-w64 GCC 16.1 (`x86_64-msvcrt-posix-seh`), static, `-municode`. Laptop: Dell Gaming 5577, i5-7300HQ, GTX 1050 4 GB (+ Intel HD 630; the user says the
  GTX runs full-time, no Optimus switching -- the adapter name at the end of the `[AFKVID] stats` line shows which chip really plays the video), AtlasOS (no HEVC extension likely).
* The AFK video must **play smoothly whatever the clip's quality / fps**, at native resolution, with hardware decode and a preload; no "PowerPoint
  slides" look when the PC is slow (a lower tier must still move). Real keyboard focus on the AFK window. Everything else is low priority.
* One exe with the video embedded. Source is split into modules (shared / core / ui / afk / media-engine), mirroring the architecture document.
* Be honest about what ran: the sandbox that wrote this had no real Windows and no GPU (see "Verified" below).
* The **strings scan** (links / malware commands inside a flagged program) is an option that is **ON by default** (`"strings"` in settings.json, "Strings scan" row on the Protection tab).
* They also asked for a **macOS Apple Silicon variant**: first taken as the strings check as a command-line tool (`mac/sentinel-strings.cpp`), then (Nov 7) as the whole app in `mac/` (menu bar, WKWebView dashboard,
  AVFoundation AFK screen; README section 12) plus a build that checks which OS it runs on (`Makefile`, `build.sh`, `shared/platform.h`). It was written without a Mac and has **never been compiled**.
* **Start with Windows, hidden in the tray** (the `autostart` switch) and **power saving after 30 minutes away** (Nov 7: "after 30 minutes afk turn laptop to power saving mode, lower power to under 30%, lower brightness").
  The second was read as: CPU maximum 25 % + display brightness at most 15 % while there has been no input for 30 min, undone at the first input. That reading was **not confirmed**: if "power" meant something else
  (the GPU?), change `shared/eco.h` and `core/eco.cpp`.
* (Nov 9-12) Put the project on GitHub (`lIIusionator/Sentinel`, MIT, `LICENSE` / `THIRD_PARTY.md`), a Windows setup (`installer/windows/sentinel.nsi`), a Mac dmg (best effort), **release v1.0** and
  **automatic updates from the repository's releases** (rule 16, README section 13). The repository must be public for the updates to work; making it public is the owner's decision.
* The Rust daemon and the named pipe of the architecture document are **not** implemented; say so, once, when it matters.

## Commands
```
mingw32-make                 # sentinel.exe (release). First build is slow: windres embeds 77 MB of video.
                             # res/afk_bg_4k60.mp4 + res/afk_bg_lite.mp4 must exist (they are in the repository: README "The two clips"; tools\make_clips.bat makes new ones)
mingw32-make TEST=1          # sentinel_test.exe  (-DSW_TEST_HOOKS; env switches in afk/afk_controller.h). Never ship it.
make tests                   # native unit tests, host g++ + bash:  62,579 (afk) + 113 (strings engine) + 21 (Mac tool, tests/mac_cli_test.sh) + 13 (autostart) + 43 (power saving) + 107 (updater) checks must pass,
                             # and on Linux / macOS 304 more for the Mac app's portable core (tests/mac_logic_tests.cpp)
make macarm                  # mac/sentinel-strings.cpp for Apple Silicon: ZIG=<path to zig 0.17> (`pip install ziglang`); never run here, only inspected (llvm-objdump)
make harness                 # harness.exe: CPU decode worker vs a scripted fake reader (Windows / Wine): 81 checks
make pathtest                # path_unicode_test.exe: non-ASCII account names: 84 checks
make idletest                # idle_cpu_test.exe (Windows / Wine): signed-file cache in isSigned + the [CPU] log line: 22 checks
make dll                     # media_engine.dll (C ABI exports MediaEngine_*)
tests/update_e2e.sh OLD NEW  # the automatic update under Wine against a fake release server (README section 13 builds the two test exes)
tests/installer_test.sh      # the Windows setup under Wine, silent (needs Wine with 32-bit support: the NSIS setup is a 32-bit program)
build_portable.bat           # Windows, no compiler needed: downloads WinLibs GCC 16.1.0 (264 MB) into C:\SentinelBuild once, then runs build.bat there.
                             # Tested only against stand-ins under Wine (tests/portable_build/README.txt); the real download/tar/GCC 16.1 build never ran here.
                             # Wine 9's cmd runs the right side of `a || b` even when `a` succeeded, so the Wine runs use tf.py to rewrite `||`; Windows is fine.
./build.sh [TEST=1]          # Linux cross build (CROSS=x86_64-w64-mingw32-); on a Mac the same command builds build-mac/SentinelWatch.app (mac/mac.mk: never run), on Windows use mingw32-make
```
Sanitizers for the native tests: add `-fsanitize=address,undefined -fno-sanitize-recover=undefined` or `-fsanitize=thread` to the `g++` line in the `tests` target.
Expect **0 warnings** with `-Wall -Wextra` (the Makefile switches off `-Wmisleading-indentation`: the code is deliberately compact).

## Rules that keep this code working
1. **Paths and streams.** Paths are `std::wstring`. For any `std::ifstream/ofstream` use `wpath(path)` (sw_common.h); never pass `toU8(path)`: the narrow
   constructors use the ANSI code page and break for Vietnamese / Japanese / accented account names (`make pathtest` guards this).
2. **Threads.** The cross compiler has the *win32* thread model (no `std::thread` / `std::mutex`; `std::shared_ptr` not thread-safe), the user's GCC is *posix*.
   Code must build under both: `CreateThread`, `SwCs/SwGuard` (shared/sw_lock.h), `std::atomic`. The CPU back-end's decode and audio workers co-own a
   `Shared` block with an intrusive refcount; keep it that way. Threads: UI thread (message loop, supervisor) / AFK controller thread (owns the window
   and the back-end) / CPU decode + audio workers / Media Foundation callbacks (atomics only).
3. **No new static imports.** D3D11, DXGI, D2D, DWrite, avrt, powrprof.dll (power plans: core/eco.cpp) and the power-setting registration are resolved at run time so the exe starts without a GPU
   stack. Check: `x86_64-w64-mingw32-objdump -p sentinel.exe | grep "DLL Name"` -- original list + GDI32 + WINMM only.
4. **Pure logic goes in `afk/afk_helpers.h` or `media-engine/src/me_util.h`** (no Windows headers) and gets a test in `tests/afk_tests.cpp`.
5. **Media engine:** every `MediaEngine_*` call from ONE thread (the AFK render thread, COM initialised). It must not block the UI thread: GPU drivers can
   take 6 s+ to create a device on this laptop; the supervisor (`afk_screen.cpp`) exists because a call can block for good.
6. **Tier memory** (`AfkTierMemory`, settings.cpp): slow verdicts need two sessions, hard failures count at once, 7-day expiry, reset by a new
   `SW_BUILD_ID` or `afkGpu` change; only the tier a player *started* on is judged (`afkJudgesTier`; `m_entryTier` is set at every preload); failures that are not the player's fault (window, monitor,
   missing clip, out of memory) are never persisted. `afk_active.flag` marks a running tier for crash detection (other builds' markers are ignored).
   **Bump `SW_BUILD_ID` (shared/version.h) when AFK behaviour changes**, so stale verdicts do not outlive their cause.
7. **Log everything the AFK screen decides**, with HRESULTs (`[AFK]`, `[AFKVID]`): it is the only way to debug on the user's PC.
8. `SW_TEST_HOOKS` code stays inside `#ifdef`; the release exe must contain none (`strings sentinel.exe | grep SW_TEST` prints nothing).
9. The CPU tier is the last resort, not the product: do not tune it at the expense of the GPU path.
10. **Idle means idle.** With the AFK screen down the app must stay near 0 % CPU (the user saw ~30 % once: Token Guard re-hashing every big signed exe in a user folder
    every 4 s, plus a 30 fps GDI+ repaint of the dashboard). Anything periodic must be cheap or cached (`isSigned` keeps "signed" until the file changes); the
    dashboard repaints only when `g_dirty` is set (anything that animates must set it each frame while it runs) or on its slow idle tick. A loop that waits on a call
    that can fail at once must sleep on failure. A `[CPU]` line in events.log (3 %+ over a minute) names the hot thread: new threads call `cpuNameThread()` first.
11. **One screen, one player -- ready BEFORE the screen, PROVEN to move, and nothing silent.**
    (a) A player that has already run a screen is never resumed: `AfkRunner::renewSlot()` (afk_tiers.cpp) drops it (`AfkSlot::used`, set in `leaveAfk()`) and the normal preload path builds a fresh one.
    That happens when the UI thread says the user has been idle for most of the wait (`AfkSnapshot::warm`, published by `updateQuoteState()`; `afkPreloadLeadMs` = half the wait, at most 30 s before the
    screen is due), **not when the screen is due**: the first version of this rule did that, so every screen after the first appeared on a player that was still loading (the clock on black, the video seconds
    late or never, and a log that only said "still loading", which does not read as a failure). A fresh player the user never saw (they came back first) is kept.
    (b) `AfkVideoGate` (afk_helpers.h, driven from `AfkRunner::frame()`): once a GPU player is ready and has been told to play, >= 4 NEW frames must arrive within 4.5 s (the engine's "blind" mode only
    starts counting after ~2 s). The clip and its sound are not revealed before that (`afkVideoStage` + hold). A failure logs `[AFK] VIDEO CHECK FAILED ... -- player: <the engine's own state>`
    (`IAfkBackend::diag()`, `MeStats`), the first one on a screen rebuilds the player (`videoCheckFailed` -> `recoverDevice`), the second goes a tier down (`fallBack(false,..)`: a strike, not a hard failure),
    and with no lower tier the screen keeps its player ("there is no lower tier"). The CPU tier has no such check.
    (c) Nothing about a screen may be silent: every `Play()` / `Pause()` result is logged and kept (`[ME] Play (...) -> hr=`), `enterAfk()`'s early returns say why (`the screen is due but cannot come up`),
    and the `screen down` line states the verdict: `video check passed N s in`, or `NO VIDEO was shown` / `NO MOVING VIDEO was shown`.
    Why: the user reported the video failing on some screens ("sometimes", the 2nd / 3rd, "or when minimized") with nothing in the log. (a) is a flaw that WAS found, by reading the Wine logs of the repeat runs
    (`screen up ... still loading` on every later screen); whether it is the whole story on the laptop is **not proven**, and (b) / (c) are the net under whatever else it is. When it happens again, read
    `screen up` (still loading?), `video check passed` / `VIDEO CHECK FAILED`, `screen down` of that screen, in that order.
12. **Strings scan.** `core/str_scan.h` is pure logic (no Windows / POSIX headers: the Windows app and `mac/sentinel-strings.cpp` both include it; tests/str_scan_tests.cpp).
    `core/str_scan.cpp` is the Windows glue: the watchers that flag an unsigned program (`file_watch`, `proc_watch` both paths, `token_guard`) call `stringsScanAsync(path)`; it queues
    (32 max), and ONE below-normal thread, alive only while the queue is not empty, waits for the file to stop growing, reads <=16 MB (first + last 8 MB), reports `[STR]` lines +
    an `STR` event + a toast, and remembers (path, size, time) so the same file is not read twice. It never runs or fetches anything and never throws out of the thread. A new pattern
    (`CMDS`, `HOT`, `NOISE`) needs a test, and a command pattern must not match what honest programs carry (the pair `a`+`b` must both be in ONE printable run). A hit is a lead, not a verdict.
13. **Power saving when away** (Nov 7, build `2026.11.07-afk4`). The rules (when it starts, when it stops, what happens when a step fails) are pure logic in `shared/eco.h` over the small `EcoSys` interface, with 43 checks in
    `tests/eco_tests.cpp` (fake plans, fake marker); `core/eco.cpp` is the Windows side. The numbers are constants in eco.h: `ECO_AFTER_MS` 30 min, `ECO_CPU_MAX_PCT` 25 (a `static_assert` keeps it under 30),
    `ECO_BRIGHT_PCT` 15. It works on a temporary COPY of the active power plan ("Sentinel Watch power saving (temporary)": CPU max 25 %, min <= 5 %, boost off, idle states allowed, brightness <= 15 %) and activates it;
    going back is "activate the user's plan, delete the copy": **the user's own plans are never edited**. `eco_active.txt` (the user's plan, the copy) is written BEFORE the copy is activated and removed after going back;
    `ecoRecover()` at the next start undoes a run that was killed, crashed or cut off (a plan that has vanished -> Windows' Balanced). `powrprof.dll` is loaded at run time by full path (rule 3).
    Away = no keyboard / mouse input for 30 min (`GetLastInputInfo`); the tick is the existing `TMR_AFK` timer in `ui/main.cpp` (1 s, rate limited in `ecoTick`). A hidden top-level window (`SentinelEnd`) hears
    `WM_ENDSESSION` (a message-only window does not) and puts the plan back before the process is gone. While it is on, `g_ecoOn` tells the AFK controller to skip the video check and the watchdog (the CPU is capped, so a
    slow start is not the player's fault; the log says so): keep that bypass if the gate or the watchdog changes. Test hooks (sentinel_test.exe only): `SW_TEST_ECO=<s>` or `<at>,<for>` (fake in-memory plans),
    `SW_TEST_ECO_REAL=1` (the real calls). Not done: looking at anything but input (a video or a download counts as away), pausing the AFK video while it is on, power calls off the UI thread, recognising a copy whose marker was deleted.
14. **Start with Windows** (Nov 7). `shared/autostart.h` is pure logic: the `Run` value is `"<exe>" --background`, and the autorun watch recognises its own entry by name AND path (the same name pointing elsewhere is
    somebody imitating it, and is reported). `autostartApply()` (core/autorun_watch.cpp) writes, heals or removes the value at start and after every toggle; `--background` = hidden in the tray, no dashboard (`ui/main.cpp`);
    starting it by hand while it runs shows the running one. tests/autostart_tests.cpp (13 checks); end to end under Wine only.
15. **macOS app** (`mac/`, README section 12). What needs no Apple framework is in `mac/core.cpp` (+ `sys.h`; tests/mac_logic_tests.cpp runs it against a fake `sw::Sys`, 304 checks, on Linux); Apple code goes ONLY into
    `apple.mm`, `app.mm` and `sys_mac.cpp`. Which OS is being built is decided in the `Makefile` (Windows_NT / Darwin / Linux), `build.sh` and `shared/platform.h` (`#error` on anything else). The Mac app has no power saving,
    no GPU tiers and no video check; its AFK screen is `AVQueuePlayer` + `AVPlayerLooper` with one log line two seconds in as the only evidence. Keep `mac/Info.plist` to the one protected folder it watches (Downloads).
    **It has never been compiled.**
16. **Automatic updates** (Nov 11, v1.0). `shared/update.h` is pure logic (version compare, release JSON validation, SHA-256, `SHA256SUMS.txt`, the "installed before" guard; 107 checks in `tests/update_tests.cpp`);
    `core/update.cpp` is the Windows side: WinHTTP loaded at run time (rule 3), one below-normal worker thread asleep on an event (rules 2 and 10: `CreateThread`, no `std::thread`, nothing periodic that costs CPU).
    Trust = HTTPS + the GitHub account (the exes are not code-signed), so `updParseRelease` stays strict: not a draft / pre-release, a plain version tag newer than `SW_VERSION`, asset addresses only under
    `https://github.com/<UPD_REPO>/releases/download/`, the exe's SHA-256 equal to its line in `SHA256SUMS.txt`. The swap runs on the UI thread (`updateTick`, the 1 s timer) only with no AFK screen, the dashboard hidden and
    input idle >= 60 s; every failing step puts the old exe back. **Release procedure:** the tag `vX.Y.Z` must equal `SW_VERSION` (the workflow checks it, or an installed copy would download the release again and again);
    assets `SentinelWatch.exe`, `SentinelWatch-Setup.exe`, `SHA256SUMS.txt`; the repository must be public (a private one answers 404). Test hooks (sentinel_test.exe only; plain http only there): `SW_TEST_UPDATE_URL`,
    `SW_TEST_UPDATE_FIRST` (ms), `SW_TEST_UPDATE_IDLE` (s). Never run: real HTTPS / TLS to api.github.com and github.com, the swap with a real antivirus holding the file, a failing swap (the rollback code has no test), a real release.

## Verified here, and not
Verified (Linux, mingw-w64 GCC 13 win32-model cross compiler, Wine 9.0, Zig 0.17 clang): clean builds, C++20/23 syntax, 62,579 native checks (also
ASAN/UBSAN/TSAN), 81 harness checks, 84 Unicode-path checks, Wine smoke runs (CPU cycle, GPU ladder, hung-driver takeover, crash marker, idle path),
import-list parity, three independent reviews (CPU back-end, controller, GPU player) with fixes applied. Wine's Media Engine does load the clip and send the events,
so the player's open / preload / first-frame code has run; it never signals new frames, which is why the Wine log always shows blind mode.
Idle-CPU fix (Oct 31): the dashboard repaint was measured under Wine (old exe ~97 % of one core with the dashboard open; new ~18 % with the dashboard in front, ~5 % behind
another window; Wine's GDI+ is not Windows' GDI+, so read it as a ratio) and `make idletest` passes. 
Repeat-screen change (Nov 2, build `2026.11.02-afk2`): several AFK periods in one Wine run (`SW_TEST_EVERY=<s>`, with `SW_TEST_AT/FOR/NOINPUT`) show a fresh player before every later
screen, `[ME] init` / `[ME] shutdown` balanced, slow verdicts counted per player (two slow screens in a row now do reach the remembered tier), and the same with the dashboard
minimized or closed (`SW_TEST_DASH=min|close`, which logs `[TEST] dashboard ...`).
Video check + early player (Nov 6, build `2026.11.06-afk3`), Wine only: (1) repeat screens (`SW_TEST_EVERY`) now find a READY player: the log shows `idle for most of the wait -- replacing the player ...`
~half a wait before the screen, then `screen up` without `still loading` (the Nov 2 build showed `still loading` on every later screen: that is how the flaw was found); the same through the real idle path
(`"afkIdleSec": 20`, no test switches: UI thread -> `AfkSnapshot::warm` -> controller) and with the dashboard minimized; (2) `SW_TEST_STUCK=<n>` (the first n players ignore the request to play):
n=1 -> `VIDEO CHECK FAILED` after 4.5 s with the engine state, a rebuilt player, `video check passed`; n=2 -> a second failure, one tier down, passes; n=9 -> three failures, `there is no lower tier`, and
`screen down ... NO MOVING VIDEO was shown`. Wine's engine counts frames only after ~2 s (blind mode), so every Wine pass is logged at 0.6-2.1 s; real hardware should pass in a fraction of a second.
Strings scan (Nov 3): 113 native checks (also ASAN/UBSAN), and end to end under Wine with the real file-watch trigger (fake droppers copied into the watched %APPDATA%): findings, order, noise,
UTF-16, tail read of a >16 MB file, clean file, rename-back not read twice, option off = silent. Four Wine runs with the page cache dropped (option on / off, alternating) all behaved the same; three earlier runs in which late directory events were missing did not reproduce, and their cause was not found (Wine-only, not proven).
Mac tool: 21 checks on Linux with the host g++ (the code is plain C++17 + std::filesystem); the Apple Silicon build (Zig 0.17, `-target aarch64-macos.11.0`) was only inspected with
llvm-objdump: Mach-O arm64, minos 11.0, libSystem only, ad-hoc signature block present.
Nov 7 (build `2026.11.07-afk4`), Linux / Wine only: start with Windows (13 checks; Wine: the `Run` value written, healed and removed, `--background` stays in the tray, a second start shows the first); power saving (43 checks, also
ASan + UBSan; Wine with fake in-memory plans: ON after the idle time, OFF about a second after real input, a killed run leaves the marker and the next start undoes it, a sign-out message puts the plan back first, an AFK screen that
starts while it is on skips the video check; the early player and the video check were run again afterwards and still behave); the Mac app's portable core (304 checks, also ASan + UBSan). The 11 switches fit the Protection tab by
geometry only (this Wine draws no text).
Nov 11 (v1.0), Linux / Wine only: updater (107 logic checks, also ASan + UBSan; Wine end to end: install, wrong checksum, cut-off download, same version, pre-release, 404, switch off, dashboard open) and the Windows
setup (silent install / uninstall under 32-bit-capable Wine; its pages were not clicked). The GitHub workflow (`.github/workflows/build.yml`) was only parsed as YAML.
**Never run:** whether the early player + video check cure the "sometimes no video" symptom on the laptop (their cause there was never reproduced), whether 4 frames in 4.5 s is the right bar on the GTX 1050
(a false FAIL costs one rebuild, then maybe a tier; the log line says which), a real GPU stuck in the way `SW_TEST_STUCK` imitates (that hook only imitates it); the strings scan on real Windows with real droppers; the Mac tool on a Mac (Gatekeeper / quarantine, `std::filesystem` there); **the Mac app** (never compiled: apple.mm, app.mm, sys_mac.cpp); the real `powrprof.dll` calls (Wine has none: whether a standard account may copy and switch power plans, whether the screen really dims and the CPU stays at 25 %, what AtlasOS's plan does with the values, how long the calls take on the UI thread); the text of the 11th Protection-tab row; real GPU decode / present of the 4K60 clip, whether renewing the player cures the 2nd-screen symptom on the laptop, how long a rebuild takes on the GTX 1050, the D2D HUD, audio, loop seam, display-off / lock, which adapter is picked, GCC 16.1 build, and the real cost of
WinVerifyTrust on 100 MB signed exes (Wine has none): that the signature cache is what removed the always-on CPU use on the laptop is a diagnosis, not a measurement.
If CPU is still high there, the `[CPU]` line in events.log names the thread.
Do not claim any of those works until the log from the real laptop says so.

## First thing to do on the real laptop
1. `mingw32-make` -- report every error / warning verbatim (GCC 16.1 has not seen this code).
2. Close the app, set `"afkIdleSec": 10` in `%LOCALAPPDATA%\SentinelWatch\settings.json`, start `sentinel.exe`, leave mouse and keyboard alone for ~15 s.
3. Read `events.log`. Wanted: `built-in 4K clip ... 3840x2160 @ 60.0 fps`, `tier 0 started`, `tier 0 is ready: first video frame ... ms`, `screen up ... tier 0` (no `still loading`),
   `video check passed: 4 new frames within ... s`, then
   `[AFKVID] stats (GPU, 3840x2160): video ~60 fps of 60.0 | display 60 fps | ... stalls over 120 ms 0 | engine dropped 0 | <adapter>`.
4. Look at it: smooth? sound? the seam at the loop point? starfield dimmed in the lens circle? Task Manager -> GPU -> *Video Decode* busy, CPU low.
5. Try the other paths: `"afkGpu": "nvidia"` vs `"intel"` (compare the stats line), `"afkMode": "cpu"` (smaller picture is fine, it must still move),
   lock the PC (Win+L) and switch the display off while the screen is up (expect `playback paused` / `playback resumed`), type into Notepad right before
   the screen appears (the first key must not reach Notepad).
6. A bad result is a `VIDEO CHECK FAILED`, `NO VIDEO was shown`, `slow window`, `tier N failed`, `hr=0x...` or `stalls over 120 ms` line: that is what to fix, with the log in hand.
7. Let it run two or three screens (set `"afkIdleSec": 10`, move the mouse, wait again) and read the `idle for most of the wait` / `screen up` / `screen down` lines of each: every later screen must come up
   without `still loading`.
8. Power saving: it needs 30 min without keyboard / mouse input (only input counts: watching a video without touching anything is "away"). To see it sooner build the test exe (`mingw32-make TEST=1`) and start it with `SW_TEST_ECO_REAL=1` and `SW_TEST_ECO=60` (real
   calls, away after 60 s). Wanted in events.log: `[ECO] power saving ON ...`, then at your first key press `[ECO] power saving OFF: back on plan ...`. While it is on, the screen is dim and Task Manager -> Performance -> CPU ->
   Speed stays low; afterwards `powercfg /list` must not show "Sentinel Watch power saving (temporary)". An `[ECO] ... failed, error 5` line means this account may not copy or switch plans; `powrprof.dll does not offer ...`
   means the functions are missing.

## Wine notes (Linux sandbox only)
`Xvfb :77`, `WINEPREFIX=...`, `DISPLAY=:77`, `LANG=C.UTF-8 LC_ALL=C.UTF-8` (a POSIX locale cannot create non-ASCII file names). Wine **ignores a `LOCALAPPDATA` given
from the shell** (use the Unicode-path test instead). Wine's Media Foundation cannot negotiate RGB32, so the CPU tier there ends in "no video"; its D3D11 is
a fake, slow NVIDIA adapter; `GetLastInputInfo` changes when a window activates. `wineserver -k` between runs; processes outlive `timeout`.

## Not done (ideas, in the order they would matter)
The Mac app: compile it and fix what the first build says; power saving, a video check and tiers there; a signed / notarised build; Endpoint Security (needs an Apple entitlement). Power saving on Windows: pausing the AFK video while
it is on, looking at more than input, power calls off the UI thread;
the strings scan reading the middle of a >16 MB file and persisting its "already read" memory; signature check in the Mac tool.
Waitable swap chain / `DXGI_ERROR` resource-lost handling beyond device loss; HUD brush reuse in the D2D overlay; two GPU back-ends alive at once; a downscaled
copy for a user's own clip; the Rust daemon and the pipe from the architecture document.
