/* media_engine.h -- Direct3D 11 + Media Foundation "Media Engine" video player behind a small C ABI.
 *
 * The engine decodes on the GPU (DXVA through a shared D3D11 device), converts + scales each new frame into a
 * back-buffer-sized texture with the GPU video processor, copies it into a flip-model swap chain and lets the host
 * draw on top (Direct2D HUD) just before Present. The CPU never touches pixels.
 *
 *   MediaEngine_Init(HWND, path)        create device + swap chain + engine, start loading (returns quickly)
 *   MediaEngine_Pump()                  housekeeping while idle/preloading (first frame capture, source retries)
 *   MediaEngine_RenderFrame()           draw one frame and Present
 *   MediaEngine_SetPlaybackState(s)     play / pause / stop
 *   MediaEngine_Shutdown()
 *
 * Threading: every function must be called from ONE thread (the render thread, COM-initialised). Only the
 * Media Foundation callback threads run concurrently, and they touch nothing but atomics.
 * The same sources build either into the host exe (default) or into media_engine.dll (-DMEDIA_ENGINE_BUILD_DLL).
 */
#ifndef SENTINEL_MEDIA_ENGINE_H
#define SENTINEL_MEDIA_ENGINE_H
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(MEDIA_ENGINE_BUILD_DLL)
#  define ME_API __declspec(dllexport)
#elif defined(MEDIA_ENGINE_USE_DLL)
#  define ME_API __declspec(dllimport)
#else
#  define ME_API
#endif

typedef enum MePlaybackState { ME_STATE_STOPPED=0, ME_STATE_PLAYING=1, ME_STATE_PAUSED=2 } MePlaybackState;

typedef enum MeStatus {
  ME_STATUS_IDLE=0,          /* not initialised */
  ME_STATUS_LOADING=1,       /* source opening / waiting for the first frame */
  ME_STATUS_READY=2,         /* first frame captured; frames can be rendered */
  ME_STATUS_ERROR=3,         /* load / decode failed (see MeStats.lastError) */
  ME_STATUS_DEVICE_LOST=4    /* GPU reset or removed: Shutdown + Init again */
} MeStatus;

/* flags returned by MediaEngine_RenderFrameEx */
#define ME_FRAME_PRESENTED   0x01u   /* Present() reached the screen */
#define ME_FRAME_NEW_VIDEO   0x02u   /* a new decoded video frame was transferred this call */
#define ME_FRAME_OCCLUDED    0x04u   /* window not visible (screen off, locked, covered): nothing was shown */
#define ME_FRAME_HAVE_VIDEO  0x08u   /* a video frame is on screen (false while only the clear colour is drawn) */

typedef struct MeConfig {
  UINT32 structSize;         /* sizeof(MeConfig) */
  UINT32 width, height;      /* swap-chain size in pixels; 0 = the window's client area */
  UINT32 syncInterval;       /* Present sync interval: 1 = every vblank, 2 = every second ... (0 -> 1) */
  BOOL   loop;               /* loop the clip (default TRUE) */
  BOOL   muted;              /* start muted (the first-frame preload is always silent) */
  float  volume;             /* 0..1 */
  double clipFps;            /* optional hint for diagnostics / fallback accounting (0 = unknown) */
  UINT32 clearColor;         /* 0xAARRGGBB drawn until the first video frame exists */
  UINT32 preferredVendor;    /* PCI vendor id of the GPU to use on a PC with more than one (0x8086 Intel, 0x10DE NVIDIA, 0x1002 AMD);
                                0 = let Windows choose (it honours the per-app graphics preference). An unknown id falls back to the default. */
} MeConfig;

typedef struct MeStats {
  UINT32  structSize;        /* set before calling MediaEngine_GetStats */
  UINT32  status;            /* MeStatus */
  UINT32  playState;         /* MePlaybackState last requested */
  UINT32  videoWidth, videoHeight;      /* native size of the clip (0 until metadata is loaded) */
  UINT32  bufferWidth, bufferHeight;    /* swap-chain size */
  double  durationSec;
  unsigned long videoFramesDelivered;   /* new decoded frames shown so far */
  unsigned long presents;               /* Present() calls that reached the screen */
  unsigned long engineFramesDropped;    /* Media Engine's own dropped-frame counter */
  HRESULT lastError;         /* last failing HRESULT (S_OK = none) */
  UINT32  vendorId, deviceId;
  UINT32  swapEffect;        /* 1 flip-discard, 2 flip-sequential, 0 bit-blit */
  UINT32  blindMode;         /* 1 = OnVideoStreamTick never signalled, frames are pulled every render */
  double  avgTransferMs, avgOverlayMs, avgPresentMs;   /* smoothed per-frame costs on the render thread */
  wchar_t adapter[96];
  wchar_t source[24];        /* how the clip was opened: "byte stream" | "path" | "url" */
  double  maxFrameGapMs;     /* longest pause between two NEW video frames since the previous MediaEngine_GetStats call (0 = none measured) */
  unsigned long frameStalls; /* pauses of more than 120 ms between two new video frames since Init (the loop point may cause one per pass) */
  /* Appended in build 2026.11.06 (a caller built against the older, shorter struct gets exactly its own size back): what a "the picture does not move" report needs. */
  UINT32  stateFlags;        /* ME_SF_* */
  UINT32  readyState;        /* the engine's own HTML-style state: 0 nothing .. 4 enough data to play through */
  double  clipTimeSec;       /* the media clock (it advances while the clip plays, with or without frames reaching us) */
  HRESULT lastPlayHr;        /* result of the latest Play() / Pause() the engine was given (S_OK before the first) */
  HRESULT lastTickHr;        /* latest OnVideoStreamTick: S_OK = a new frame was ready, S_FALSE = none yet, else the failure */
  UINT32  renderErrors;      /* STREAMRENDERINGERROR events seen */
  unsigned long transferFailures;   /* TransferVideoFrame calls that failed */
} MeStats;
#define ME_SF_STARTED     0x001u   /* the clip loaded and the silent preload play was issued */
#define ME_SF_HAVE_FRAME  0x002u   /* the first frame is captured */
#define ME_SF_WANT_PLAY   0x004u   /* the host asked for playback */
#define ME_SF_PLAY_ISSUED 0x008u   /* Play() is in effect (not paused by us) */
#define ME_SF_PAUSED      0x010u   /* the engine itself reports "paused" */
#define ME_SF_ENDED       0x020u   /* the engine itself reports "ended" */
#define ME_SF_MUTED       0x040u
#define ME_SF_BLIND       0x080u   /* frames derived from the media clock (OnVideoStreamTick never signalled) */

/* Host overlay hook: called on the render thread with the freshly copied video already in the back buffer.
 * op == ME_OVERLAY_DRAW    : draw onto `backBuffer` (ID3D11Texture2D*, BGRA, not AddRef'd; valid for this call only)
 * op == ME_OVERLAY_RELEASE : drop every reference to back-buffer textures / the device (before resize and shutdown) */
#define ME_OVERLAY_DRAW     1u
#define ME_OVERLAY_RELEASE  2u
typedef struct MeOverlayArgs {
  UINT32 structSize;
  UINT32 op;
  void*  d3dDevice;          /* ID3D11Device* */
  void*  backBuffer;         /* ID3D11Texture2D* (DRAW only) */
  UINT32 width, height;
  UINT32 videoFlags;         /* ME_FRAME_* bits describing this frame (DRAW only) */
} MeOverlayArgs;
typedef void (CALLBACK *MeOverlayProc)(void* user, const MeOverlayArgs* args);

typedef void (CALLBACK *MeLogProc)(const wchar_t* line);

/* -- lifecycle ------------------------------------------------------------------------------------------------- */
ME_API void    MediaEngine_SetLogger(MeLogProc proc);                       /* optional; default OutputDebugString */
ME_API HRESULT MediaEngine_Init(HWND hwnd, const wchar_t* videoPath);       /* defaults: loop, muted, window-size buffers */
ME_API HRESULT MediaEngine_InitEx(HWND hwnd, const wchar_t* videoPath, const MeConfig* cfg);
ME_API void    MediaEngine_Shutdown(void);

/* -- per-frame ------------------------------------------------------------------------------------------------- */
ME_API HRESULT MediaEngine_Pump(void);                                      /* S_OK ready, S_FALSE still loading, else failure */
ME_API HRESULT MediaEngine_RenderFrame(void);                               /* = RenderFrameEx(NULL, NULL, NULL) */
ME_API HRESULT MediaEngine_RenderFrameEx(MeOverlayProc overlay, void* user, UINT32* flagsOut);
                                                                            /* S_OK presented, S_FALSE occluded, else failure */
ME_API HRESULT MediaEngine_Resize(UINT32 width, UINT32 height);

/* -- control --------------------------------------------------------------------------------------------------- */
ME_API void    MediaEngine_SetPlaybackState(int state);                     /* MePlaybackState */
ME_API void    MediaEngine_SetVolume(float volume, BOOL muted);
ME_API void    MediaEngine_SetSyncInterval(UINT32 syncInterval);
ME_API BOOL    MediaEngine_GetStats(MeStats* out);                          /* render thread */

#ifdef __cplusplus
}
#endif
#endif /* SENTINEL_MEDIA_ENGINE_H */
