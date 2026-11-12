# Sentinel Watch Suite

A cross-platform system-monitoring suite for Windows and macOS, written primarily in C++17 with native platform components. Sentinel monitors processes, startup items and files, and provides an event dashboard and AFK screensaver. Windows also includes an optional strings scan for links and suspicious commands in flagged programs.

> **Project status:** Windows and macOS builds are not code-signed. The macOS app compiles in CI but has not yet been run on a Mac. Windows GPU playback and power-management behavior also need validation on real hardware. See [Known limitations](#known-limitations).

## Project contributors

| Contributor | Role |
|---|---|
| <img src="https://github.com/lIIusionator.png?size=32" width="32" height="32" alt="" /> [lIIusionator](https://github.com/lIIusionator) | Author and maintainer |
| <img src="https://github.com/JustZeal.png?size=32" width="32" height="32" alt="" /> [JustZeal](https://github.com/JustZeal) | Co-author |

## Overview

- Process, startup-item and file monitoring with an event dashboard.
- Program trust checks and a configurable strings scan. The strings scan is enabled by default on supported builds; results are indicators, not proof of malicious activity.
- Windows tray application with optional startup launch, automatic updates and power saving while idle.
- AFK screensaver with looping video, audio, clock and quote. The Windows executable embeds two video clips.
- Separate macOS app with a menu-bar interface and platform-native monitoring components.

### Platform support

| Platform | Availability |
|---|---|
| Windows 10/11, x64 | Main application; release installer and standalone executable available. |
| macOS, Apple Silicon and Intel | App build supported; runtime behavior has not yet been verified on a Mac. |
| Linux | No native app. Linux can cross-compile the Windows executable; use it on Windows. |

## Get a release

Download assets from [Latest release](https://github.com/lIIusionator/Sentinel/releases/latest).

- **Windows:** Run `SentinelWatch-Setup.exe` for a per-user installation, or download `SentinelWatch.exe` and run it from a writable folder. The app is unsigned, so SmartScreen may show a warning. The app starts in the tray by default; double-click its icon to open the dashboard.
- **macOS:** If the release includes `SentinelWatch-macOS.dmg`, open it and copy `SentinelWatch.app` to Applications. The app is ad-hoc signed, not notarized, and has not been run on a Mac; use with care.
- **Linux:** There is no Linux release. See [Build on Linux](#linux-windows-cross-build) to cross-compile the Windows executable.

Release assets include `SHA256SUMS.txt`. On Windows, check a downloaded executable with `Get-FileHash .\SentinelWatch.exe -Algorithm SHA256` and compare the result with its entry in that file.

## Development setup

### Repository checkout

The bundled clips are large assets required by the Windows build. Clone the full repository before building:

```sh
git clone https://github.com/lIIusionator/Sentinel.git
cd Sentinel
```

### Windows build

Use 64-bit MinGW-w64 with `g++` and `windres` available on `PATH`:

```bat
mingw32-make
```

This produces `sentinel.exe`. To compile the test-hook build, use `mingw32-make TEST=1`; **do not distribute `sentinel_test.exe`**. If you do not have a compiler, `build_portable.bat` downloads a portable toolchain and builds without installing it.

### Linux (Windows cross-build)

Install MinGW-w64 and the usual build tools (`make`, `bash`), then run:

```sh
./build.sh
```

The output is `sentinel.exe`, not a native Linux application. Set `CROSS=x86_64-w64-mingw32-` if your toolchain requires it.

### macOS build

Install Apple's command-line developer tools, then run:

```sh
xcode-select --install
./build.sh
```

The app bundle is created at `build-mac/SentinelWatch.app`. The build targets the current Mac architecture; use `make ARCH=x86_64` to build for Intel on Apple Silicon. The app compiles in CI but has not yet been smoke-tested on real macOS hardware.

## Tests

Run the repository's native test suite with:

```sh
make tests
```

The tests cover portable logic, including AFK timing and tier selection, strings scanning, startup behavior, update rules and the macOS app's portable core. Platform-specific behavior should also be tested on the corresponding real operating system; Wine results do not replace Windows or macOS hardware testing.

Other build and test targets are documented in the Makefile and `CLAUDE.md`. Do not ship test builds or enable test hooks in release builds.

## Architecture

The code is organized around shared portable logic and OS-specific integrations:

| Directory | Purpose |
|---|---|
| `shared/` | Common utilities, settings, platform selection, startup and power-saving rules, and update logic. |
| `core/` | Windows monitoring, trust checks, event handling, strings scanning and updater integration. |
| `ui/` | Windows tray, dashboard, notifications and graphics. |
| `afk/` | Windows AFK screen lifecycle, video backends, assets and portable rendering/timing logic. |
| `media-engine/` | D3D11 and Media Foundation video player exposed through a C ABI. |
| `mac/` | macOS app, system integration, dashboard and command-line strings scanner. |
| `tests/` | Portable unit tests, platform test harnesses and update/installer test scripts. |
| `installer/` | Windows installer and macOS disk-image packaging scripts. |
| `res/` | Windows resources, avatar and bundled AFK clips. |

Windows settings and logs are stored under `%LOCALAPPDATA%\SentinelWatch`. On macOS, the data directory is `~/Library/Application Support/SentinelWatch`; `$SW_DATA_DIR` can override it. The Windows AFK delay is configured by `afkIdleSec` in `settings.json` (`0` uses the Windows screen-saver wait if configured, otherwise five minutes).

## Security and updates

The Windows updater downloads releases from this repository and verifies the executable against the release's SHA-256 manifest before installation. It is enabled by default and can be disabled in the Updates tab or settings.

The applications are **not code-signed**. HTTPS and a release checksum help detect transfer errors or a mismatched download, but cannot protect against a compromised GitHub account or malicious release. Only install releases you trust. Automatic updating requires this repository to be public.

The strings scan reads file content only; it does not execute programs or fetch detected links. Findings are leads for investigation, not a malware verdict. Scans are bounded in size and may not see packed, encrypted or non-text content.

## Known limitations

- The Windows executable has not been validated on real Windows hardware with a real GPU. Smooth 4K playback, audio, GPU selection, power-plan changes and display brightness need testing on actual devices.
- The macOS app compiled on GitHub's macOS runner but has **never been launched on a Mac**. Runtime behavior, permissions, Gatekeeper handling and disk-image installation are not yet confirmed.
- The macOS build is ad-hoc signed and not notarized. It does not include the Windows automatic updater or power-saving feature.
- The Rust daemon and named pipe from the original architecture design are not implemented; monitoring runs in the application.

## License and third-party assets

Source code is available under the [MIT License](LICENSE). The bundled video clips and avatar image are not covered by that license; see [`THIRD_PARTY.md`](THIRD_PARTY.md) for their terms.
