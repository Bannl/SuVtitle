@echo off
rem build_timecode_smoke.cmd [--no-heavy] -- U3 (2026-11-02, the TRANSCRIPTION -- docs/PROGRESS_U3.md section 4).
rem timecode_smoke = app\SourceTimecode.h (the SOURCE timecode a recording carries: BWF / RF64 / BW64 + iXML, MOV / MP4 tmcd,
rem MXF) at /W4, no /utf-8, like the product. TWO builds of the same source: timecode_smoke.exe (/O2) and timecode_fuzz.exe
rem (AddressSanitizer + the debug CRT's checked iterators, /Od: the same checks, its fuzz-ish truncations, lying sizes and
rem byte flips included -- an out-of-bounds read or write aborts it). The ffmpeg-made fixtures (make_tc_fixtures.js, real
rem files written by a real muxer): SUVTITLE_TC_FIXTURES when set, else tests\cpp\work\tcfix\, made there once when ffmpeg
rem and node are present (nothing downloaded); without them the smoke prints NOT CHECKED for that part. Output:
rem tests\cpp\work\ (ignored). --no-heavy: the /O2 build only.
setlocal
cd /d "%~dp0"
call "%~dp0..\..\tools\vsenv.cmd" || exit /b 1
if not exist work\obj_timecode mkdir work\obj_timecode
if not exist work\obj_timecode_asan mkdir work\obj_timecode_asan
cl /nologo /std:c++17 /EHsc /W4 /MD /O2 ^
   /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   timecode_smoke.cpp /Fe:work\timecode_smoke.exe /Fo:work\obj_timecode\ || exit /b 1
if /i not "%~1"=="--no-heavy" (
  cl /nologo /std:c++17 /EHsc /W4 /MDd /Od /Zi /fsanitize=address /D_ITERATOR_DEBUG_LEVEL=2 ^
     /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
     timecode_smoke.cpp /Fe:work\timecode_fuzz.exe /Fo:work\obj_timecode_asan\ /Fd:work\obj_timecode_asan\ || exit /b 1
)
set "TCDIR=%SUVTITLE_TC_FIXTURES%"
if "%TCDIR%"=="" if exist "%~dp0work\tcfix\notc.mp4" set "TCDIR=%~dp0work\tcfix"
if "%TCDIR%"=="" (
  where node >nul 2>nul && node "%~dp0make_tc_fixtures.js" "%~dp0work\tcfix" && set "TCDIR=%~dp0work\tcfix"
)
cd /d "%~dp0work"
if "%TCDIR%"=="" (
  "%~dp0work\timecode_smoke.exe" || exit /b 1
) else (
  "%~dp0work\timecode_smoke.exe" "%TCDIR%" || exit /b 1
)
if /i "%~1"=="--no-heavy" exit /b 0
if "%TCDIR%"=="" (
  "%~dp0work\timecode_fuzz.exe"
) else (
  "%~dp0work\timecode_fuzz.exe" "%TCDIR%"
)
exit /b %errorlevel%
