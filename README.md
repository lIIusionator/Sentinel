# Sentinel Watch Suite

## Authors

<div align="left">
  <a href="https://github.com/lIIusionator"><img src="https://github.com/lIIusionator.png?size=40" width="40" height="40" alt="lIIusionator" /></a>
  <a href="https://github.com/JustZeal"><img src="https://github.com/JustZeal.png?size=40" width="40" height="40" alt="JustZeal" /></a>
</div>

- [lIIusionator](https://github.com/lIIusionator) — Author
- [JustZeal](https://github.com/JustZeal) — Co-author

A small system-monitoring suite for Windows and macOS. Sentinel watches processes, files and startup items, and includes an AFK screensaver. Windows also has an optional strings scan for links and suspicious commands inside flagged programs; it is on by default.

Windows ships as one executable with both video clips embedded. The macOS version is a separate app. Sentinel is MIT-licensed; the bundled video clips and avatar image have separate terms (`THIRD_PARTY.md`).

> **Please read:** Windows and Mac builds are not code-signed. The macOS app has compiled in CI but has not yet been run on a Mac. See [Known limitations](#known-limitations).

## Install

Get release downloads from [Latest release](https://github.com/lIIusionator/Sentinel/releases/latest). Choose the steps for your operating system:

### Windows 10/11

1. Download **`SentinelWatch-Setup.exe`** from the release assets and run it. It installs just for your Windows account under `%LOCALAPPDATA%\Programs\SentinelWatch`; administrator rights are not required.
2. If SmartScreen warns that the app is unrecognized, choose **More info → Run anyway**. This warning appears because the app is not code-signed.
3. Launch **Sentinel Watch** from the Start menu. It starts hidden in the system tray by default; double-click its tray icon to open the dashboard.

Prefer not to install? Download **`SentinelWatch.exe`**, put it in a folder you can write to, and run it. The installer and standalone executable are both listed in `SHA256SUMS.txt`; verify either with:

```powershell
Get-FileHash .\SentinelWatch.exe -Algorithm SHA256
```

Compare the result with the matching entry in `SHA256SUMS.txt`.

### macOS (Apple Silicon and Intel)

1. Download **`SentinelWatch-macOS.dmg`** from a release that includes it, open the disk image, and drag **SentinelWatch.app** to **Applications**.
2. If macOS blocks the downloaded app, Control-click it in Applications, choose **Open**, and confirm. This build is ad-hoc signed, not notarized; macOS may require an extra approval.
3. Open Sentinel Watch. It appears in the menu bar. On first access to Downloads, macOS asks whether it may monitor that folder; allow access if you want that folder watched.

The Mac app is an early, **not yet run on real macOS** build. Expect runtime issues and review [Known limitations](#known-limitations) before relying on it.

### Linux

**There is no native Linux Sentinel app.** Linux can be used to cross-compile the Windows executable, but that does not make it a Linux application.

To build the Windows version from Linux:

1. Install a MinGW-w64 cross-compiler (`x86_64-w64-mingw32-g++`) and the usual build tools (`make`, `bash`).
2. Clone this repository, including its large video assets, and enter the directory:
   ```sh
   git clone https://github.com/lIIusionator/Sentinel.git
   cd Sentinel
   ```
3. Build:
   ```sh
   ./build.sh
   ```
   The output is `sentinel.exe`. Transfer it to a Windows PC to use it. The AFK clips are embedded in the executable.

Running the Windows build under Wine is useful for some tests, but is not a supported Linux installation. For the native macOS app, build on macOS instead; see [Build from source](#build-from-source).

## What it does

- **Watches** processes, startup entries and selected files for suspicious changes.
- **Checks trust** of programs and can inspect strings in flagged files for links and suspicious commands. The strings scan is enabled by default and can be turned off in Protection settings.
- **Shows alerts and an event log** in the dashboard.
- **Can start with Windows** in the tray and check for Windows updates from this repository's releases.
- **Offers an AFK screensaver** with looping video, sound, a clock and a quote.
- **On Windows, can save power while you're away:** after 30 minutes without input it temporarily limits CPU performance and dims the screen, then restores your power plan when you return. This behavior is enabled by default and can be switched off.

## The AFK screen: an extremely serious feature list

- Plays a giant, cinematic video while you are away, because apparently the desktop needed a screensaver with a budget.
- Includes a smaller video for computers that would rather not render the director's cut.
- Adds a clock, a quote, a starfield and a dramatic lens vignette. Your monitor is now a tiny planetarium with opinions.
- Notices when you come back and gets out of your way. It has learned basic manners.
- On Windows, tries the GPU first and steps down to lighter playback if the machine struggles. No promise that your GPU won't make its own artistic choices.
- Comes with two clips already inside the Windows executable. You can also drop a supported video into `%LOCALAPPDATA%\SentinelWatch\content\afk`.

The Windows AFK wait can be configured in `%LOCALAPPDATA%\SentinelWatch\settings.json` with `afkIdleSec` (`0` uses the Windows screen-saver wait if configured, otherwise five minutes; `10` is handy for testing). The app writes diagnostics to `%LOCALAPPDATA%\SentinelWatch\events.log`.

## Build from source

The repository includes both bundled video clips (`res/afk_bg_4k60.mp4` and `res/afk_bg_lite.mp4`); they make the initial Windows resource build large and slow.

### Windows

Use 64-bit MinGW-w64 with `g++` and `windres` on `PATH`:

```bat
mingw32-make
```

This produces `sentinel.exe`. To build the test-only executable, use `mingw32-make TEST=1`; do not distribute `sentinel_test.exe`. If you do not have a compiler, `build_portable.bat` downloads a portable toolchain and builds without installing it. See `CLAUDE.md` for detailed build and test commands.

### Linux (Windows cross-build)

Install MinGW-w64 and run:

```sh
./build.sh
```

This creates `sentinel.exe`, not a native Linux program. Set `CROSS=x86_64-w64-mingw32-` if needed by your toolchain.

### macOS (native app)

Install Apple's command-line developer tools, then from the repository directory run:

```sh
xcode-select --install
./build.sh
```

The app bundle is created at `build-mac/SentinelWatch.app`. The build selects Apple Silicon or Intel according to the Mac; use `make ARCH=x86_64` to build Intel on Apple Silicon. The Mac app has not yet been smoke-tested on real hardware.

## Settings and data

- **Windows:** `%LOCALAPPDATA%\SentinelWatch` (settings, logs and user-provided AFK videos).
- **macOS:** `~/Library/Application Support/SentinelWatch` (the `$SW_DATA_DIR` environment variable can override this location).
- Settings are stored in `settings.json`. Close Sentinel before editing it; the app may rewrite its settings while running.

Windows settings include switches for monitoring, strings scanning, startup behavior, power saving and updates. macOS has its own set of supported switches; Windows-only features such as power saving and automatic updates are not available there.

## Updates and trust

On Windows, automatic updates are enabled by default and are downloaded from this repository's GitHub releases. The updater checks the downloaded executable against the release's SHA-256 manifest and installs it when the app is idle. You can turn this off in the Updates tab or settings.

The executable is **not code-signed**. HTTPS and the published checksum help detect transfer errors or a mismatched download, but they do not protect against a compromised GitHub account or malicious release. Only install releases you trust. Automatic updating requires this repository to be public.

## Known limitations

- The Windows executable has not been validated on real Windows hardware with a real GPU. In particular, smooth 4K playback, audio, GPU selection, power-plan changes and display brightness need testing on actual devices.
- The macOS app compiled on GitHub's macOS runner but has **never been launched on a Mac**. Its runtime behavior, permissions, Gatekeeper handling and disk image installation are not yet confirmed.
- The macOS release is ad-hoc signed and not notarized. The app does not have the Windows version's automatic updater or power-saving feature.
- The Rust daemon and named pipe described in the original architecture design are not implemented; monitoring runs in the app.
- Bundled video clips and the avatar image are not covered by the MIT license; see `THIRD_PARTY.md`.

## License

Sentinel's source code is available under the [MIT License](LICENSE), subject to the third-party asset terms in `THIRD_PARTY.md`.
