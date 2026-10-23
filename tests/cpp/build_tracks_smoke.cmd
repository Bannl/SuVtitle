@echo off
rem build_tracks_smoke.cmd [--no-heavy] -- ported 2026-10-23 from AudioPB's scratchpad\ui_test\build_tracks_smoke.cmd.
rem tracks_smoke = app\MediaTracks.h (wave WH: the film's own tracks) at /W4, no /utf-8, like the product. TWO builds of
rem the same source: tracks_smoke.exe (/O2: every check, the film's timings, a light fuzz) and tracks_fuzz.exe
rem (AddressSanitizer + the debug CRT's checked iterators, /Od: the HEAVY truncation + bit-flip fuzz -- an out-of-bounds
rem read or write aborts it; ~20 min). Test data: AudioPB's WH set in <SUVTITLE_TEST_MEDIA>\wh\ (not in the repository;
rem without the variable the smoke prints NOT CHECKED and exits 0). The maintainer's film (SUVTITLE_TEST_FILM) is opened READ ONLY
rem (skipped when absent). MLang (SubtitleReader.h) needs ole32. Output: tests\cpp\work\ (ignored).
rem --no-heavy: the /O2 build and its checks only (the ASan fuzz is skipped).
setlocal
cd /d "%~dp0"
call "%~dp0..\..\tools\vsenv.cmd" || exit /b 1
if not exist work\obj_tracks mkdir work\obj_tracks
if not exist work\obj_tracks_asan mkdir work\obj_tracks_asan
cl /nologo /std:c++17 /EHsc /W4 /MD /O2 ^
   /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   tracks_smoke.cpp /Fe:work\tracks_smoke.exe /Fo:work\obj_tracks\ /link ole32.lib || exit /b 1
if /i not "%~1"=="--no-heavy" (
  cl /nologo /std:c++17 /EHsc /W4 /MDd /Od /Zi /fsanitize=address /D_ITERATOR_DEBUG_LEVEL=2 ^
     /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
     tracks_smoke.cpp /Fe:work\tracks_fuzz.exe /Fo:work\obj_tracks_asan\ /Fd:work\obj_tracks_asan\ /link ole32.lib || exit /b 1
)
cd /d "%~dp0work"
"%~dp0work\tracks_smoke.exe" || exit /b 1
if /i "%~1"=="--no-heavy" exit /b 0
"%~dp0work\tracks_fuzz.exe" --fuzz
exit /b %errorlevel%
