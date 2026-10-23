@echo off
rem build_subreader_smoke.cmd -- ported 2026-10-23 from AudioPB's scratchpad\ui_test\build_subreader_smoke.cmd (641f1aa).
rem subreader_smoke = app\SubtitleReader.h (header-only) compiled ALONE, at /W4, WITHOUT /utf-8 -- exactly how the
rem product compiles it. MLang needs COM: ole32. Output: tests\cpp\work\ (ignored).
setlocal
cd /d "%~dp0"
call "%~dp0..\..\tools\vsenv.cmd" || exit /b 1
if not exist work\obj_subreader mkdir work\obj_subreader
cl /nologo /std:c++17 /EHsc /W4 /MD /O2 ^
   /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   subreader_smoke.cpp /Fe:work\subreader_smoke.exe /Fo:work\obj_subreader\ /link ole32.lib || exit /b 1
cd /d "%~dp0work"
"%~dp0work\subreader_smoke.exe"
exit /b %errorlevel%
