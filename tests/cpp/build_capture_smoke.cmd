@echo off
rem build_capture_smoke.cmd [--quick] [--only <TAG>] -- wave L1b (2026-10-25): the smoke of the LIVE capture hub.
rem capture_smoke = capture\capture_smoke.cpp + the hub's OWN sources (engine\TrCapture{Hub,Core,Enum,Wasapi,Wav}.cpp)
rem + engine\TrDecimator.cpp, compiled together with the product's flags (/MT like the engine: no VC++ redistributable;
rem UNICODE, WIN32_LEAN_AND_MEAN, NOMINMAX, no /utf-8) but at /W4 (the engine itself is held to /W3). It does NOT depend
rem on engine\main_translator.cpp: a fake ILiveSink / ILiveEvents stands in for the engine. capture_tone.exe (the
rem application whose sound [REAL] captures through "one application's loopback") is built next to it.
rem Output + run folder: tests\cpp\work\capture\ (ignored; the smoke writes its generated media in .\capture_media\).
rem Arguments go to the smoke: --quick = everything on the virtual clock, no real device, no real-clock run (~1 min);
rem --only <TAG> = one section (C1 ARITH FIFO STATE REFUSE LEVELS SET STOP C2 C3 C4 C5 RESYNC OVF REAL, and the fix
rem round's TREE BACKOFF LIST);
rem no argument = the full suite (real clock + this PC's devices: about 10 minutes). C2 needs SUVTITLE_TEST_MEDIA
rem (bench_en.wav / bench_zh.wav, 16 kHz; NOT CHECKED without it). The smoke NEVER opens an input (in:).
setlocal
cd /d "%~dp0"
call "%~dp0..\..\tools\vsenv.cmd" || exit /b 1
if not exist work\capture\obj mkdir work\capture\obj
cl /nologo /std:c++17 /EHsc /W4 /MT /O2 ^
   /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   capture\capture_tone.cpp /Fe:work\capture\capture_tone.exe /Fo:work\capture\obj\ /link ole32.lib || exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /MT /O2 ^
   /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I"..\..\engine" /I"..\..\common" ^
   capture\capture_smoke.cpp ^
   "..\..\engine\TrCaptureHub.cpp" "..\..\engine\TrCaptureCore.cpp" "..\..\engine\TrCaptureEnum.cpp" ^
   "..\..\engine\TrCaptureWasapi.cpp" "..\..\engine\TrCaptureWav.cpp" "..\..\engine\TrDecimator.cpp" ^
   /Fe:work\capture\capture_smoke.exe /Fo:work\capture\obj\ ^
   /link ole32.lib avrt.lib mmdevapi.lib version.lib propsys.lib || exit /b 1
cd /d "%~dp0work\capture"
"%~dp0work\capture\capture_smoke.exe" %*
exit /b %errorlevel%
