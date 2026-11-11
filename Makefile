# Makefile -- Sentinel Watch Suite, MinGW-w64 (64-bit).
#
#   Windows (MinGW in PATH):   mingw32-make
#   Linux cross-compile:       make CROSS=x86_64-w64-mingw32- -j8
#
#   make              sentinel.exe  (everything in one file, the two AFK clips embedded)
#   make dll          media_engine.dll -- the GPU video player on its own, behind the C ABI of media-engine/include/media_engine.h
#   make TEST=1       sentinel_test.exe -- same sources plus -DSW_TEST_HOOKS (environment switches for smoke tests)
#   make tests        native unit tests for the pure logic, the Mac command-line tool and (Linux / macOS) the Mac app's core (needs a host g++ and bash, not the cross compiler)
#   make macarm       the Mac command-line strings check (mac/) cross-built for Apple Silicon with Zig (ZIG=path/to/zig); on a Mac see mac/sentinel-strings.cpp
#   make harness      harness.exe -- drives the CPU renderer's decode worker against a scripted fake video reader (run it on Windows or under Wine)
#   make pathtest     path_unicode_test.exe -- log / settings / data folder under accented, Vietnamese, Japanese and emoji account names (Windows or Wine)
#   make idletest     idle_cpu_test.exe -- the signature cache (core/trust.cpp) and the [CPU] log line (shared/sw_common.cpp) (Windows or Wine)
# --- Which OS is this? Decided here, before anything is compiled -------------------------------------------------------------------
#   Windows (MinGW's mingw32-make, OS=Windows_NT)   builds sentinel.exe        the rules below
#   macOS   (uname -s = Darwin)                      builds SentinelWatch.app   mac/mac.mk (Apple clang from the Xcode command line tools)
#   Linux                                            cross-compiles sentinel.exe with CROSS=x86_64-w64-mingw32- (./build.sh does that); the Mac app needs a Mac
# Anything else stops with a message. The C++ sources check the same thing (shared/platform.h).
HOST_OS := $(if $(filter Windows_NT,$(OS)),Windows,$(shell uname -s 2>/dev/null))
ifeq ($(filter Windows Darwin Linux,$(HOST_OS)),)
$(error Unsupported OS "$(HOST_OS)": Sentinel Watch builds on Windows (MinGW-w64) and macOS (Apple clang); Linux cross-compiles the Windows exe)
endif
$(info [OS] $(HOST_OS))
ifeq ($(HOST_OS),Darwin)
include mac/mac.mk
else

CROSS   ?=
CXX      = $(CROSS)g++
WINDRES  = $(CROSS)windres
BUILD   ?= build
OUT     ?= sentinel.exe

CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -Wno-misleading-indentation \
            -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
            -Ishared -Icore -Iui -Iafk -Imedia-engine/include -Imedia-engine/src -Ithird_party -Ires
# header dependencies: an edited header rebuilds every object that includes it (without this a stale object can survive a header change)
DEPFLAGS = -MMD -MP
ifeq ($(TEST),1)
CXXFLAGS += -DSW_TEST_HOOKS
OUT       = sentinel_test.exe
BUILD     = build-test
endif

# GDI+ and Media Foundation are linked normally; Direct3D / DXGI / Direct2D / DirectWrite / avrt are resolved at run time
# (so the exe still starts, and falls back to the CPU renderer, on a machine without a working GPU stack).
LDFLAGS ?= -static -static-libgcc -static-libstdc++ -mwindows -municode
LIBS     = -lgdiplus -lgdi32 -lole32 -loleaut32 -luuid -lmfplat -lmf -lmfreadwrite -lmfuuid -lwinmm \
           -lshell32 -luser32 -ladvapi32 -lcrypt32 -lwintrust -lsecur32 -lwbemuuid

SRC_SHARED = shared/sw_common.cpp shared/settings.cpp
SRC_CORE   = core/autorun_watch.cpp core/eco.cpp core/events.cpp core/file_watch.cpp core/proc_watch.cpp core/response.cpp core/str_scan.cpp core/token_guard.cpp core/trust.cpp \
             core/update.cpp
SRC_UI     = ui/content.cpp ui/dashboard.cpp ui/gfx.cpp ui/main.cpp ui/toast.cpp
SRC_AFK    = afk/afk_assets.cpp afk/afk_cpu_backend.cpp afk/afk_gpu_backend.cpp afk/afk_loop.cpp afk/afk_overlay_d2d.cpp \
             afk/afk_screen.cpp afk/afk_state.cpp afk/afk_tiers.cpp afk/afk_win.cpp
SRC_ENGINE = media-engine/src/media_engine.cpp
SRC        = $(SRC_SHARED) $(SRC_CORE) $(SRC_UI) $(SRC_AFK) $(SRC_ENGINE)
OBJ        = $(patsubst %.cpp,$(BUILD)/%.o,$(SRC))
RES        = $(BUILD)/res/sentinel.res.o

# The two AFK clips are not in the source zip (74 MB). Say so before anything is compiled, whenever the exe is what is being built.
CLIPS      = res/afk_bg_4k60.mp4 res/afk_bg_lite.mp4
CLIP_HELP  = The two AFK clips are not in the source zip (74 MB): see the section "The two clips" in README.md
CLIP_MISSING = $(strip $(foreach c,$(CLIPS),$(if $(wildcard $(c)),,$(c))))
ifneq ($(and $(CLIP_MISSING),$(filter-out clean dll tests harness pathtest idletest,$(or $(MAKECMDGOALS),all))),)
$(error Cannot build the exe, not found: $(CLIP_MISSING) -- $(CLIP_HELP))
endif

all: $(OUT)

$(OUT): $(OBJ) $(RES)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

-include $(OBJ:.o=.d)

# (the parse-time check above catches a missing clip first; this rule is the fallback if one disappears later)
$(CLIPS):
	$(error Not found: $@ -- $(CLIP_HELP))

# the resource compile is the slow part (77 MB of video): it only reruns when the .rc or a clip changes
$(RES): res/sentinel.rc res/avatar.jpg $(CLIPS)
	@mkdir -p $(dir $@)
	$(WINDRES) -I res -O coff res/sentinel.rc -o $@

# --- the GPU video player as a stand-alone DLL (same sources as the in-exe copy) ---------------------------------------
dll: media_engine.dll
media_engine.dll: media-engine/src/media_engine.cpp
	$(CXX) $(CXXFLAGS) -DMEDIA_ENGINE_BUILD_DLL -shared -static -static-libgcc -static-libstdc++ -o $@ $< -lole32 -loleaut32 -luuid -lmfplat -lmf -lmfuuid

# --- native unit tests (host compiler) ------------------------------------------------------------------------------------
tests:
	@mkdir -p $(BUILD)
	g++ -std=c++17 -O1 -g -Wall -Wno-misleading-indentation -Iafk -Imedia-engine/src tests/afk_tests.cpp -o $(BUILD)/afk_tests -pthread
	$(BUILD)/afk_tests
	g++ -std=c++17 -O1 -g -Wall -Wno-misleading-indentation -Icore tests/str_scan_tests.cpp -o $(BUILD)/str_scan_tests
	$(BUILD)/str_scan_tests
	bash tests/mac_cli_test.sh $(BUILD)
	g++ -std=c++17 -O1 -g -Wall -Wextra -Ishared tests/autostart_tests.cpp -o $(BUILD)/autostart_tests
	$(BUILD)/autostart_tests
	g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-misleading-indentation -Ishared tests/eco_tests.cpp -o $(BUILD)/eco_tests
	$(BUILD)/eco_tests
	g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-misleading-indentation -Ishared tests/update_tests.cpp -o $(BUILD)/update_tests
	$(BUILD)/update_tests

# the Mac app's portable core (mac/core.cpp: settings, event feed, trust rules, watchers, strings scan, LaunchAgent, quotes, state JSON) against a fake operating system.
# Plain C++17 + POSIX, so it runs on Linux; Windows (MinGW) has no flock / getpwuid, so it is left out of `tests` there.
ifeq ($(HOST_OS),Linux)
tests: mactests
mactests:
	@mkdir -p $(BUILD)
	g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-misleading-indentation -Icore -Imac -Ishared tests/mac_logic_tests.cpp mac/core.cpp -pthread -o $(BUILD)/mac_logic_tests
	$(BUILD)/mac_logic_tests
.PHONY: mactests
endif

# --- the macOS (Apple Silicon) command-line strings check: not run here, only built (no Mac in the sandbox that wrote it) -------
# -target ...macos.11.0: runs on every Apple Silicon macOS (11 Big Sur and later); Zig's own default would ask for macOS 15
ZIG ?= zig
macarm: mac/sentinel-strings.cpp mac/scanfile.h core/str_scan.h
	@mkdir -p $(BUILD)
	$(ZIG) c++ -target aarch64-macos.11.0 -std=c++17 -O2 -Icore -Imac -o $(BUILD)/sentinel-strings-macos-arm64 $<

# --- the decode-worker harness (Media Foundation attribute / sample objects only; no codec, no GPU) ------------------------
harness: harness.exe
harness.exe: tests/cpu_decode_harness.cpp afk/afk_cpu_backend.cpp afk/afk_helpers.h
	$(CXX) $(CXXFLAGS) -o $@ tests/cpu_decode_harness.cpp -static -static-libgcc -static-libstdc++ \
	  -lgdiplus -lgdi32 -lole32 -loleaut32 -luuid -lmfplat -lmf -lmfreadwrite -lmfuuid -lwinmm

# --- the data folder / log / settings code against non-ASCII account names (Vietnamese, Japanese, emoji) ------------------
pathtest: path_unicode_test.exe
path_unicode_test.exe: tests/path_unicode_test.cpp shared/sw_common.cpp shared/settings.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^ -static -static-libgcc -static-libstdc++ -lole32 -lshell32 -luser32 -ladvapi32

# --- the idle-CPU fix: signed-file cache + per-thread CPU line (a stand-in replaces WinVerifyTrust, hence SW_TEST_HOOKS) -----
idletest: idle_cpu_test.exe
idle_cpu_test.exe: tests/idle_cpu_test.cpp core/trust.cpp shared/sw_common.cpp
	$(CXX) $(CXXFLAGS) -DSW_TEST_HOOKS -o $@ $^ -static -static-libgcc -static-libstdc++ -lwintrust -lcrypt32 -lole32 -lshell32 -luser32 -ladvapi32

clean:
	rm -rf build build-test sentinel.exe sentinel_test.exe media_engine.dll harness.exe path_unicode_test.exe idle_cpu_test.exe

.PHONY: all dll tests macarm harness pathtest idletest clean

endif   # HOST_OS is not Darwin
