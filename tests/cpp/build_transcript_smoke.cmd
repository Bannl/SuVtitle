@echo off
rem build_transcript_smoke.cmd -- U3 (2026-11-02, the TRANSCRIPTION): transcript_smoke = app\DocxWriter.cpp ALONE + the
rem speaker transcript's builder (buildSpeakerTranscript), its own checks of the stored XML, then the external check
rem (transcript_check.py: ZIP + XML well-formedness + the named styles + the turns) with the isolated Python interpreter.
rem Writes transcript_out.docx and transcript_tc_out.docx in tests\cpp\work\ (ignored).
setlocal
cd /d "%~dp0"
call "%~dp0..\..\tools\vsenv.cmd" || exit /b 1
call "%~dp0..\..\tools\pyenv.cmd" || exit /b 1
if not exist work\obj_transcript mkdir work\obj_transcript
cl /nologo /std:c++17 /EHsc /W4 /MD /O2 ^
   /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   transcript_smoke.cpp "..\..\app\DocxWriter.cpp" /Fe:work\transcript_smoke.exe /Fo:work\obj_transcript\ || exit /b 1
cd /d "%~dp0work"
"%~dp0work\transcript_smoke.exe" || exit /b 1
%PYRUN% "%~dp0transcript_check.py" "%~dp0work\transcript_out.docx" --turns 4 --speakers "Speaker 1,Marie,Speaker 3,Speaker 4" || exit /b 1
%PYRUN% "%~dp0transcript_check.py" "%~dp0work\transcript_tc_out.docx" --turns 2 --no-names --source-tc
exit /b %errorlevel%
