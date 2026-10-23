@echo off
rem build_pipe_smoke.cmd -- S-HOST fix round (2026-10-23). pipe_smoke = the REAL app\TranslatorManager.cpp + a scripted
rem engine (pipe\fake_engine.cpp) built as SuVtitleEngine.exe NEXT TO IT in tests\cpp\work\pipe\ (the manager spawns
rem <its own folder>\SuVtitleEngine.exe). The proof of REVIEW_RV2_App.md RV2-1 / RV2-2 / RV2-5 (~45 s). Both console
rem exes, no window: the manager spawns the engine with CREATE_NO_WINDOW. Pipes named after the test's own process.
setlocal
cd /d "%~dp0"
call "%~dp0..\..\tools\vsenv.cmd" || exit /b 1
if not exist work\pipe\obj mkdir work\pipe\obj
cl /nologo /std:c++17 /EHsc /W4 /MD /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I"..\..\common" pipe\fake_engine.cpp /Fe:work\pipe\SuVtitleEngine.exe /Fo:work\pipe\obj\ || exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /MD /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I"..\..\app" /I"..\..\common" pipe\pipe_smoke.cpp "..\..\app\TranslatorManager.cpp" ^
   /Fe:work\pipe\pipe_smoke.exe /Fo:work\pipe\obj\ || exit /b 1
cd /d "%~dp0work\pipe"
"%~dp0work\pipe\pipe_smoke.exe"
exit /b %errorlevel%
