@echo off
REM build_tools.cmd -- builds real_tools.exe (+ its canary.exe copy is made by the driver) -- /W4, no /utf-8, like the
REM product. Ported 2026-10-23 from AudioPB's wg_real\build_tools.cmd (641f1aa). Output: work\ (git-ignored).
setlocal
cd /d "%~dp0"
call "%~dp0..\..\..\tools\vsenv.cmd" || exit /b 1
if not exist work\obj mkdir work\obj
cl /nologo /std:c++17 /EHsc /W4 /MD /O2 ^
   /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I"..\..\..\common" ^
   real_tools.cpp /Fe:work\real_tools.exe /Fo:work\obj\ /link user32.lib shell32.lib ole32.lib || exit /b 1
exit /b 0
