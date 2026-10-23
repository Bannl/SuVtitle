@echo off
rem build_wg_smoke.cmd -- ported 2026-10-23 from AudioPB's scratchpad\ui_test\build_wg_smoke.cmd (641f1aa).
rem wg_smoke = app\JsonR.h, app\TrLaunch.h, app\TrAssoc.h at /W4, no /utf-8, like the product. The registry part
rem writes ONLY under HKCU\Software\SuVtitle-Test\smoke-<pid> and deletes it. Output: tests\cpp\work\ (ignored).
setlocal
cd /d "%~dp0"
call "%~dp0..\..\tools\vsenv.cmd" || exit /b 1
if not exist work\obj_wg mkdir work\obj_wg
cl /nologo /std:c++17 /EHsc /W4 /MD /O2 ^
   /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I"..\..\common" ^
   wg_smoke.cpp /Fe:work\wg_smoke.exe /Fo:work\obj_wg\ /link user32.lib shell32.lib ole32.lib advapi32.lib || exit /b 1
cd /d "%~dp0work"
"%~dp0work\wg_smoke.exe"
exit /b %errorlevel%
