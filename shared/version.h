// version.h -- one place for the version strings shown in the UI and written to the log (Windows app and Mac app).
#pragma once
#ifndef SW_VERSION_OVERRIDE
#define SW_VERSION    "v1.0"                    // the release tag must be the same (the release workflow checks it); the updater compares it with the latest release
#else
#define SW_VERSION    SW_VERSION_OVERRIDE       // test builds only: a second exe with another version, for the update test (tests/update_e2e.sh)
#endif
#define SW_BUILD_ID   "2026.11.11-v1.0"
#define SW_WIDE2(x)   L##x
#define SW_WIDE(x)    SW_WIDE2(x)
#define SW_VERSION_W  SW_WIDE(SW_VERSION)
#define SW_BUILD_ID_W SW_WIDE(SW_BUILD_ID)
