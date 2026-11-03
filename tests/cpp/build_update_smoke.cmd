@echo off
rem build_update_smoke.cmd -- NEW (03/11/2026, THE UPDATE BANNER): update_smoke = app\NextaleUpdate.cpp (the manifest, the
rem version rule, the URL guard, SuVtitle's page address) and AppConfig.h's UPDATECHECK / UPDATESKIP, RUN -- the network half
rem against a loopback server the smoke starts (127.0.0.1): no test contacts nextale-audio.com.
rem /W4, no /utf-8, like the product. Output: tests\cpp\work\ (ignored). Sentinels: cmut_upd.cmd.
setlocal
cd /d "%~dp0"
call "%~dp0..\..\tools\vsenv.cmd" || exit /b 1
if not exist work\obj_update mkdir work\obj_update
cl /nologo /std:c++17 /EHsc /W4 /MD /O2 ^
   /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I"..\..\common" ^
   update_smoke.cpp ..\..\app\NextaleUpdate.cpp /Fe:work\update_smoke.exe /Fo:work\obj_update\ /link ws2_32.lib winhttp.lib shell32.lib user32.lib || exit /b 1
cd /d "%~dp0work"
"%~dp0work\update_smoke.exe"
exit /b %errorlevel%
