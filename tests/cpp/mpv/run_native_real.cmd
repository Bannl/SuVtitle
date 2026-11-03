@echo off
rem run_native_real.cmd -- N1 (2026-10-26): the REAL app with the native video engine (build\x64-Debug, or SUV_N1_CONFIG), run
rem as a TEST INSTANCE (--test-instance suvn1) and driven by native_real.js: posted mouse / keys to ITS window, CDP for its
rem page, PrintWindow captures. Builds native_tool.exe and the fake engine (tests\cpp\pipe\fake_engine.cpp, SUV_FAKE_MODE=live:
rem no model, no CPU analysis) into work\real\bin, next to a copy of the app (SuVtitle.exe, WebView2Loader.dll, web\, mpv\).
rem Needs: the build (tools\buildlock.cmd x64-Debug, with SUVTITLE_MPV_DIR so build\x64-Debug\mpv\libmpv-2.dll exists) and
rem ffmpeg on PATH the first time (the media, work\real\media). The window goes on the SECOND screen (app.txt WINPOS), is never
rem activated, the profile is ISOLATED (work\real\profile), mpv is silent (a test instance), CDP port 9391. Full screen is
rem entered once, for less than 25 s. Results: work\real\runs\N.jsonl + PNG captures. Exit 0 = every check passed.
setlocal
call "%~dp0..\..\..\tools\vsenv.cmd" || exit /b 1
set "R=%~dp0..\..\.."
set "B=%~dp0work\real\bin"
if not exist "%B%" mkdir "%B%"
if not exist "%~dp0work\real\obj" mkdir "%~dp0work\real\obj"
cd /d "%~dp0work\real\obj" || exit /b 1
set "FLAGS=/nologo /std:c++17 /EHsc /W4 /MD /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS"
cl %FLAGS% "%~dp0native_tool.cpp" /Fe:"%B%\native_tool.exe" /Fo:.\ || exit /b 1
cl %FLAGS% /I"%R%\common" "%R%\tests\cpp\pipe\fake_engine.cpp" /Fe:"%B%\SuVtitleEngine.exe" /Fo:.\ || exit /b 1
node "%~dp0native_real.js"
exit /b %errorlevel%
