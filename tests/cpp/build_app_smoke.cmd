@echo off
rem build_app_smoke.cmd -- NEW (S-HOST 2026-10-23): app_smoke = what SuVtitle added (AudioPBImport.h, AppConfig.h, the
rem engine.txt pair, TrAssoc's unregister, TrLaunch's --register-player / "SVTP"), at /W4, no /utf-8, like the product.
rem Scratch trees under %TEMP% (deleted), the registry under HKCU\Software\SuVtitle-Test\appsmoke-<pid> (deleted).
setlocal
cd /d "%~dp0"
call "%~dp0..\..\tools\vsenv.cmd" || exit /b 1
if not exist work\obj_app mkdir work\obj_app
cl /nologo /std:c++17 /EHsc /W4 /MD /O2 ^
   /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I"..\..\common" ^
   app_smoke.cpp /Fe:work\app_smoke.exe /Fo:work\obj_app\ /link user32.lib shell32.lib shlwapi.lib ole32.lib advapi32.lib || exit /b 1
cd /d "%~dp0work"
"%~dp0work\app_smoke.exe"
exit /b %errorlevel%
