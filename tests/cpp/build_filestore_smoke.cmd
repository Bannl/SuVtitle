@echo off
rem build_filestore_smoke.cmd -- ported 2026-10-23 from AudioPB's scratchpad\ui_test\build_filestore_smoke.cmd (641f1aa).
rem filestore_smoke = app\TrFileStore.h (header-only) + common\TrPipe.h, at /W4, without /utf-8 like the product. It
rem uses a private folder under %TEMP% and deletes it. Output: tests\cpp\work\ (ignored).
setlocal
cd /d "%~dp0"
call "%~dp0..\..\tools\vsenv.cmd" || exit /b 1
if not exist work\obj_filestore mkdir work\obj_filestore
cl /nologo /std:c++17 /EHsc /W4 /MD /O2 ^
   /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I"..\..\common" ^
   filestore_smoke.cpp /Fe:work\filestore_smoke.exe /Fo:work\obj_filestore\ /link user32.lib || exit /b 1
cd /d "%~dp0work"
"%~dp0work\filestore_smoke.exe"
exit /b %errorlevel%
