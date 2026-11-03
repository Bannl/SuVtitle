@echo off
rem run_live_e2e.cmd -- the REAL Live window: build\x64-Debug\SuVtitle.exe --live as a TEST INSTANCE (--test-instance suvl2) against the FAKE
rem engine's `live` mode (tests\cpp\pipe\fake_engine.cpp, SUV_FAKE_MODE=live), driven through CDP (live_e2e.js). L2 step 8, 2026-10-25.
rem   set SUV_L2=<a work folder, git-ignored: bin\ obj\ profile\ shots\ work\ export\ are made in it>   then   run_live_e2e.cmd
rem Needs build\x64-Debug (tools\buildlock.cmd x64-Debug first). Builds the fake engine (as SuVtitleEngine.exe) and real_tools.exe into
rem %SUV_L2%\bin, then runs live_e2e.js. Windows go on the SECOND screen (LIVEPOS / WINPOS at x < 0), never activated; the profile is
rem ISOLATED under %SUV_L2%\profile (the product refuses a test instance on the real one); CDP port 9381; screenshots in %SUV_L2%\shots.
rem The Export's OS Save dialog (D1-D7) runs only with SUV_E2E_EXPORT=1: Windows may bring it to the foreground when the user is idle.
rem Exit 0 = every check passed.
setlocal
if "%SUV_L2%"=="" ( echo run_live_e2e: set SUV_L2 to a work folder first & exit /b 2 )
call "%~dp0..\..\..\tools\vsenv.cmd" || exit /b 1
set "R=%~dp0..\..\.."
if not exist "%SUV_L2%\bin" mkdir "%SUV_L2%\bin"
if not exist "%SUV_L2%\obj" mkdir "%SUV_L2%\obj"
cd /d "%SUV_L2%\obj" || exit /b 1
set "FLAGS=/nologo /std:c++17 /EHsc /W4 /MD /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS"
cl %FLAGS% /I"%R%\common" "%R%\tests\cpp\pipe\fake_engine.cpp" /Fe:"%SUV_L2%\bin\SuVtitleEngine.exe" /Fo:.\ || exit /b 1
cl %FLAGS% /I"%R%\common" "%R%\tests\e2e\real\real_tools.cpp" /Fe:"%SUV_L2%\bin\real_tools.exe" /Fo:.\ /link user32.lib shell32.lib ole32.lib || exit /b 1
node "%~dp0live_e2e.js"
exit /b %errorlevel%
