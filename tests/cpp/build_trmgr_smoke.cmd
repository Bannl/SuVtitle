@echo off
rem build_trmgr_smoke.cmd -- ported 2026-10-23 from AudioPB's scratchpad\ui_test\trmgr\build_trmgr_smoke.cmd (641f1aa).
rem trmgr_smoke = the REAL app\TranslatorManager.cpp + a scripted engine (trmgr\fake_helper.cpp) built as
rem SuVtitleEngine.exe NEXT TO IT in tests\cpp\work\trmgr\ (the manager spawns <its own folder>\SuVtitleEngine.exe).
rem Both console exes, no window: the manager spawns the engine with CREATE_NO_WINDOW.
setlocal
cd /d "%~dp0"
call "%~dp0..\..\tools\vsenv.cmd" || exit /b 1
if not exist work\trmgr\obj mkdir work\trmgr\obj
cl /nologo /std:c++17 /EHsc /W4 /MD /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I"..\..\common" trmgr\fake_helper.cpp /Fe:work\trmgr\SuVtitleEngine.exe /Fo:work\trmgr\obj\ || exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /MD /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I"..\..\app" /I"..\..\common" trmgr\trmgr_smoke.cpp "..\..\app\TranslatorManager.cpp" ^
   /Fe:work\trmgr\trmgr_smoke.exe /Fo:work\trmgr\obj\ || exit /b 1
cd /d "%~dp0work\trmgr"
"%~dp0work\trmgr\trmgr_smoke.exe"
exit /b %errorlevel%
