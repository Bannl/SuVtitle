@echo off
rem build_player_smoke.cmd -- ported 2026-10-23 from AudioPB's scratchpad\ui_test\build_player_smoke.cmd (641f1aa).
rem player_smoke = app\TrPlayer.h (the player's core, header-only) driven against a FAKE host -- the bridge in a box.
rem /W4, no /utf-8, like the product. It reads we_vectors.txt NEXT TO ITS EXE: tests\web\we_vectors.txt (S-WEB's -- the
rem ONE file the page's harness reads too: the C++/JS parity) is copied there first. Output: tests\cpp\work\ (ignored).
rem (TrPlayer.h includes MediaTracks.h -> SubtitleReader.h -> MLang: ole32)
setlocal
cd /d "%~dp0"
call "%~dp0..\..\tools\vsenv.cmd" || exit /b 1
if not exist work\obj_player mkdir work\obj_player
copy /y "..\web\we_vectors.txt" "work\we_vectors.txt" >nul || exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /MD /O2 ^
   /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I"..\..\common" ^
   player_smoke.cpp /Fe:work\player_smoke.exe /Fo:work\obj_player\ /link user32.lib shlwapi.lib ole32.lib || exit /b 1
cd /d "%~dp0work"
"%~dp0work\player_smoke.exe"
exit /b %errorlevel%
