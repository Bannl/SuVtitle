@echo off
rem build_filedocx_smoke.cmd -- ported 2026-10-23 from AudioPB's scratchpad\ui_test\build_filedocx_smoke.cmd (641f1aa).
rem filedocx_smoke = app\DocxWriter.cpp ALONE + the file-transcript builder, then the external check (ZIP + XML
rem well-formedness + paragraphs) with the isolated Python interpreter. Writes its two .docx in tests\cpp\work\.
setlocal
cd /d "%~dp0"
call "%~dp0..\..\tools\vsenv.cmd" || exit /b 1
call "%~dp0..\..\tools\pyenv.cmd" || exit /b 1
if not exist work\obj_filedocx mkdir work\obj_filedocx
cl /nologo /std:c++17 /EHsc /W4 /MD /O2 ^
   /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   filedocx_smoke.cpp "..\..\app\DocxWriter.cpp" /Fe:work\filedocx_smoke.exe /Fo:work\obj_filedocx\ || exit /b 1
cd /d "%~dp0work"
"%~dp0work\filedocx_smoke.exe" || exit /b 1
%PYRUN% "%~dp0filedocx_check.py" "%~dp0work\filedocx_out.docx"
exit /b %errorlevel%
