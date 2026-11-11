// version.h -- one place for the version strings shown in the UI and written to the log (Windows app and Mac app).
#pragma once
#define SW_VERSION    "v1.7"
#define SW_BUILD_ID   "2026.11.07-afk4"
#define SW_WIDE2(x)   L##x
#define SW_WIDE(x)    SW_WIDE2(x)
#define SW_VERSION_W  SW_WIDE(SW_VERSION)
#define SW_BUILD_ID_W SW_WIDE(SW_BUILD_ID)
