@echo off
rem make_clips.bat <your video> -- makes the two clips that build.bat / make embed into sentinel.exe:
rem     res\afk_bg_4k60.mp4   3840x2160 @ 60 fps, H.264 (High, level 5.2) + the original audio     (tier 0, GPU)
rem     res\afk_bg_lite.mp4   1920x1080 @ 30 fps, H.264 (High, level 4.2) + the original audio     (tier 1 GPU, tier 2 CPU)
rem
rem These are exactly the settings the built-in clips were made with, from the 4K 60 fps clip you sent (an HEVC file; ffmpeg decodes HEVC by itself,
rem the Windows HEVC extension is not needed here). H.264 is used because it plays everywhere, HEVC does not.
rem Needs ffmpeg with libx264 on PATH. Run it from the project folder:   tools\make_clips.bat C:\path\to\your_video.mp4
rem A source that is not 3840x2160 @ 60 fps still works (the first clip keeps the source size and frame rate; the second is scaled to 1080p30).
setlocal
if "%~1"=="" ( echo Usage: tools\make_clips.bat "C:\path\to\video.mp4" & exit /b 1 )
if not exist "%~1" ( echo Cannot find "%~1" & exit /b 1 )
where ffmpeg >nul 2>nul || ( echo ffmpeg is not on PATH & exit /b 1 )
if not exist res ( echo Run this from the project folder ^(the one that contains res\^) & exit /b 1 )

echo [1/2] res\afk_bg_4k60.mp4 -- slow preset, takes a while ...
ffmpeg -hide_banner -loglevel warning -stats -y -i "%~1" -map 0:v:0 -map 0:a:0 -sn -dn -map_metadata -1 ^
  -c:v libx264 -preset slow -crf 18 -tune animation -profile:v high -level 5.2 -pix_fmt yuv420p ^
  -g 120 -keyint_min 24 -bf 3 -refs 4 ^
  -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv ^
  -fps_mode passthrough -c:a copy -movflags +faststart res\afk_bg_4k60.mp4 || exit /b 1

echo [2/2] res\afk_bg_lite.mp4 ...
ffmpeg -hide_banner -loglevel warning -stats -y -i "%~1" -map 0:v:0 -map 0:a:0 -sn -dn -map_metadata -1 ^
  -vf "scale=1920:1080:flags=lanczos,fps=30" ^
  -c:v libx264 -preset slow -crf 22 -tune animation -profile:v high -level 4.2 -pix_fmt yuv420p ^
  -g 60 -keyint_min 24 -bf 2 -refs 3 ^
  -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv ^
  -c:a copy -movflags +faststart res\afk_bg_lite.mp4 || exit /b 1

echo Done: res\afk_bg_4k60.mp4 and res\afk_bg_lite.mp4. Now run build.bat or mingw32-make.
