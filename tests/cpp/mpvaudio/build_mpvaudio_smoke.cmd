@echo off
rem build_mpvaudio_smoke.cmd [--bench <file>...] -- N3 (2026-10-26): the analysis' sound through libmpv -- engine\TrMpvAudio.cpp
rem and the libmpv path of engine\TrMedia.cpp -- against the REAL libmpv-2.dll. Needs SUVTITLE_MPV_DIR = a folder holding
rem libmpv-2.dll and include\mpv\client.h (the variable the app's build reads too: README "Building"), Python 3 and ffmpeg
rem the first time (make_media.py makes the clips in work\media, git-ignored). The Vulkan loader sections need
rem SUVTITLE_VULKAN_RT = the extracted LunarG "VulkanRT-X64-<version>-Components" folder (its x64\vulkan-1.dll; NOT CHECKED
rem without it). mpv is HEADLESS here (no window, no sound device). The product's flags (/MT, UNICODE, no /utf-8), at /W4.
rem Output and run folder: work\. Exit 0 = every check passed (2 = not run: no SUVTITLE_MPV_DIR / no media / no libmpv).
rem --bench <file>... = the whole 16 kHz decode of each file, libmpv then Media Foundation: speed and CPU (no verdict).
setlocal
cd /d "%~dp0"
if "%SUVTITLE_MPV_DIR%"=="" ( echo build_mpvaudio_smoke: set SUVTITLE_MPV_DIR to the folder of libmpv-2.dll first & exit /b 2 )
if not exist "%SUVTITLE_MPV_DIR%\include\mpv\client.h" ( echo build_mpvaudio_smoke: no include\mpv\client.h in "%SUVTITLE_MPV_DIR%" & exit /b 2 )
call "%~dp0..\..\..\tools\vsenv.cmd" || exit /b 1
call "%~dp0..\..\..\tools\pyenv.cmd" || exit /b 1
if not exist work\obj mkdir work\obj
%PYRUN% "%~dp0make_media.py" "%~dp0work\media" || exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /MT /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I"%SUVTITLE_MPV_DIR%\include" /I"..\..\..\engine" ^
   mpvaudio_smoke.cpp "..\..\..\engine\TrMedia.cpp" /Fe:work\mpvaudio_smoke.exe /Fo:work\obj\ ^
   /link mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib wmcodecdspuuid.lib ws2_32.lib user32.lib || exit /b 1
cd /d "%~dp0work"
"%~dp0work\mpvaudio_smoke.exe" %*
exit /b %errorlevel%
