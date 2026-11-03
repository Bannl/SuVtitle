@echo off
rem build_mpv_smoke.cmd -- N1 (2026-10-26): the smoke of app\MpvPlayer.cpp (the native video engine's wrapper) against the
rem REAL libmpv-2.dll. Needs SUVTITLE_MPV_DIR = a folder holding libmpv-2.dll and include\mpv\client.h (the variable the app's
rem CMake build reads too: README "Building"), and ffmpeg on PATH the first time (it makes the test media in
rem work\media\, git-ignored: a 10 s 320x180 60 fps H.264 picture with TWO lossless sound tracks, 440 Hz and 1320 Hz at half
rem scale, and a file that is not media at all; since the fix round also an mp3 with a cover picture and a flac).
rem Optional: SUVTITLE_SMOKE_CODECS = a folder of samples -> [CODECS] opens and plays each one.
rem The test windows go on the SECOND screen (the monitor at -1920,0 when there is one), never activated; mpv is SILENT (ao=null;
rem what is heard is checked through a file: ao=pcm). /W4, no /utf-8, like the product. Output and run folder: work\.
rem Exit 0 = every check passed (2 = not run: no SUVTITLE_MPV_DIR / no media).
setlocal
cd /d "%~dp0"
if "%SUVTITLE_MPV_DIR%"=="" ( echo build_mpv_smoke: set SUVTITLE_MPV_DIR to the folder of libmpv-2.dll first & exit /b 2 )
if not exist "%SUVTITLE_MPV_DIR%\include\mpv\client.h" ( echo build_mpv_smoke: no include\mpv\client.h in "%SUVTITLE_MPV_DIR%" & exit /b 2 )
call "%~dp0..\..\..\tools\vsenv.cmd" || exit /b 1
if not exist work\obj mkdir work\obj
if not exist work\media mkdir work\media
if not exist work\media\main60.mkv (
  ffmpeg -y -hide_banner -loglevel error -f lavfi -i testsrc2=s=320x180:r=60:d=10 ^
    -f lavfi -i "aevalsrc=0.5*sin(2*PI*440*t):s=48000:d=10" -f lavfi -i "aevalsrc=0.5*sin(2*PI*1320*t):s=48000:d=10" ^
    -map 0:v -map 1:a -map 2:a -c:v libx264 -preset ultrafast -g 60 -keyint_min 60 -sc_threshold 0 -pix_fmt yuv420p ^
    -c:a flac -sample_fmt s16 -metadata:s:a:0 title=A440 -metadata:s:a:1 title=B1320 work\media\main60.mkv || exit /b 1
)
if not exist work\media\notmedia.mkv ( echo This is not a media file.> work\media\notmedia.mkv )
rem (fix round) a sound file with a COVER (an attached picture: mpv lists it as a video track, albumart) and a plain flac
if not exist work\media\cover.mp3 (
  ffmpeg -y -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=3" ^
    -f lavfi -i "color=c=red:s=64x64:r=1:d=1" -map 0:a -map 1:v -c:a libmp3lame -q:a 6 -c:v mjpeg ^
    -disposition:v:0 attached_pic -id3v2_version 3 work\media\cover.mp3 || exit /b 1
)
if not exist work\media\tone.flac (
  ffmpeg -y -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=3" -c:a flac ^
    work\media\tone.flac || exit /b 1
)
cl /nologo /std:c++17 /EHsc /W4 /MD /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I"%SUVTITLE_MPV_DIR%\include" mpv_smoke.cpp /Fe:work\mpv_smoke.exe /Fo:work\obj\ /link user32.lib gdi32.lib ws2_32.lib || exit /b 1
cd /d "%~dp0work"
"%~dp0work\mpv_smoke.exe"
exit /b %errorlevel%
