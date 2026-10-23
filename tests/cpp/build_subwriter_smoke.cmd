@echo off
rem build_subwriter_smoke.cmd -- ported 2026-10-23 from AudioPB's scratchpad\ui_test\build_subwriter_smoke.cmd (641f1aa).
rem subwriter_smoke = app\SubtitleWriter.h (header-only, pure) compiled ALONE, at /W4, WITHOUT /utf-8 -- exactly how
rem the product compiles it. Output: tests\cpp\work\ (ignored).
setlocal
cd /d "%~dp0"
call "%~dp0..\..\tools\vsenv.cmd" || exit /b 1
if not exist work\obj_subwriter mkdir work\obj_subwriter
cl /nologo /std:c++17 /EHsc /W4 /MD /O2 ^
   /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   subwriter_smoke.cpp /Fe:work\subwriter_smoke.exe /Fo:work\obj_subwriter\ || exit /b 1
cd /d "%~dp0work"
"%~dp0work\subwriter_smoke.exe"
exit /b %errorlevel%
