# mac.mk -- the macOS build. The Makefile includes this file when `uname -s` says Darwin (see "Which OS is this?" there); nothing of the Windows rules is read on a Mac.
# Needs Apple clang from the Xcode command line tools: xcode-select --install   (no full Xcode, no Homebrew, no Zig)
#
#   make                 build-mac/SentinelWatch.app    arm64 on Apple Silicon (x86_64 on an Intel Mac: the default follows `uname -m`), ad-hoc signed
#   make ARCH=x86_64     the other architecture (clang cross-builds it; an Apple Silicon Mac needs Rosetta only to RUN it)
#   make run             build, then open the app
#   make tests           the unit tests with the host compiler: 304 checks of the portable core + the strings engine + the command-line tool + autostart
#   make clean
# The AFK clips are copied into the app when res/afk_bg_4k60.mp4 and res/afk_bg_lite.mp4 exist (README "The two clips"); without them the AFK screen has no video.
# Compiled without warnings by GitHub's macOS runner (the release workflow) but never RUN by whoever wrote these rules (no Mac in the sandbox): the portable core is tested, apple.mm / app.mm are not.

ARCH     ?= $(shell uname -m)
MACMIN   ?= 11.0
MAC_CXX  ?= clang++
OUT      := build-mac
APP      := $(OUT)/SentinelWatch.app
VER      := $(shell sed -n 's/.*SW_VERSION *"v\([0-9.]*\)".*/\1/p' shared/version.h | head -1)
MACFLAGS := -std=c++17 -O2 -arch $(ARCH) -mmacosx-version-min=$(MACMIN) -Wall -Wextra -Wno-misleading-indentation -Wno-unused-parameter -Icore -Imac -Ishared -MMD -MP
FRAMEWORKS := -framework Cocoa -framework WebKit -framework AVFoundation -framework QuartzCore -framework CoreServices -framework CoreGraphics -framework Foundation
OBJ      := $(OUT)/core.o $(OUT)/sys_mac.o $(OUT)/apple.o $(OUT)/app.o
CONTENT  := $(wildcard mac/content/*.txt)

all: $(APP)

$(OUT):
	mkdir -p $(OUT)
$(OUT)/%.o: mac/%.cpp | $(OUT)
	$(MAC_CXX) $(MACFLAGS) -c $< -o $@
$(OUT)/%.o: mac/%.mm | $(OUT)
	$(MAC_CXX) $(MACFLAGS) -fobjc-arc -c $< -o $@

$(APP): $(OBJ) mac/Info.plist mac/ui/dashboard.html res/avatar.jpg $(CONTENT) shared/version.h
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources/ui $(APP)/Contents/Resources/content
	$(MAC_CXX) $(MACFLAGS) $(OBJ) $(FRAMEWORKS) -pthread -o $(APP)/Contents/MacOS/SentinelWatch
	sed 's/@VERSION@/$(VER)/g' mac/Info.plist > $(APP)/Contents/Info.plist
	cp mac/ui/dashboard.html res/avatar.jpg $(APP)/Contents/Resources/ui/
	cp $(CONTENT) $(APP)/Contents/Resources/content/
	@for f in res/afk_bg_4k60.mp4 res/afk_bg_lite.mp4; do if [ -f $$f ]; then cp $$f $(APP)/Contents/Resources/; else echo "note: $$f is missing: the AFK screen will have no video"; fi; done
	codesign --force --sign - $(APP)
	@echo "built $(APP) (arch $(ARCH), macOS $(MACMIN)+). Start it: open $(APP)"

run: $(APP)
	open $(APP)

tests: | $(OUT)
	$(MAC_CXX) -std=c++17 -O1 -g -Wall -Wextra -Wno-misleading-indentation -Icore -Imac -Ishared tests/mac_logic_tests.cpp mac/core.cpp -pthread -o $(OUT)/mac_logic_tests
	$(OUT)/mac_logic_tests
	$(MAC_CXX) -std=c++17 -O1 -g -Wall -Wno-misleading-indentation -Icore tests/str_scan_tests.cpp -o $(OUT)/str_scan_tests
	$(OUT)/str_scan_tests
	bash tests/mac_cli_test.sh $(OUT)
	$(MAC_CXX) -std=c++17 -O1 -g -Wall -Wextra -Ishared tests/autostart_tests.cpp -o $(OUT)/autostart_tests
	$(OUT)/autostart_tests

clean:
	rm -rf $(OUT)

-include $(OBJ:.o=.d)
.PHONY: all run tests clean
