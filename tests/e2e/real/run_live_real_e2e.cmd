@echo off
rem run_live_real_e2e.cmd -- the REAL Live mode end to end: build\x64-Debug\SuVtitle.exe as a TEST INSTANCE (--test-instance suvl4) with the REAL ENGINE
rem (CPU models) and the real capture hub fed by `wav:` inputs, driven through CDP (live_real_e2e.js). L4, 2026-10-25. Sibling of run_live_e2e.cmd (L2: fake engine).
rem   set SUV_L4=<a work folder, git-ignored: bin\ obj\ profile\ media\ shots\ logs\ work\ are made in it>
rem   set SUVTITLE_PAYLOAD=<a folder holding whisper\ llama\ models\ sherpa\>      set SUVTITLE_TEST_MEDIA=<a folder with bench_en.wav bench_zh.wav filemode\meeting.wav>
rem   run_live_real_e2e.cmd [live | film | smoke | all]       (default all; smoke = no model, no CPU gate)
rem Needs build\x64-Debug (tools\buildlock.cmd x64-Debug first). Builds real_tools.exe into %SUV_L4%\bin, then runs live_real_e2e.js. Windows go on the SECOND
rem screen, never activated; the profile is ISOLATED under %SUV_L4%\profile; CDP port 9384. Exit 0 = every check passed.
setlocal
if "%SUV_L4%"=="" ( echo run_live_real_e2e: set SUV_L4 to a work folder first & exit /b 2 )
if "%SUVTITLE_PAYLOAD%"=="" ( echo run_live_real_e2e: set SUVTITLE_PAYLOAD first & exit /b 2 )
if "%SUVTITLE_TEST_MEDIA%"=="" ( echo run_live_real_e2e: set SUVTITLE_TEST_MEDIA first & exit /b 2 )
call "%~dp0..\..\..\tools\vsenv.cmd" || exit /b 1
set "R=%~dp0..\..\.."
if not exist "%SUV_L4%\bin" mkdir "%SUV_L4%\bin"
if not exist "%SUV_L4%\obj" mkdir "%SUV_L4%\obj"
cd /d "%SUV_L4%\obj" || exit /b 1
set "FLAGS=/nologo /std:c++17 /EHsc /W4 /MD /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS"
cl %FLAGS% /I"%R%\common" "%R%\tests\e2e\real\real_tools.cpp" /Fe:"%SUV_L4%\bin\real_tools.exe" /Fo:.\ /link user32.lib shell32.lib ole32.lib || exit /b 1
node "%~dp0live_real_e2e.js" %*
exit /b %errorlevel%
