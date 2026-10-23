@echo off
REM run_real.cmd -- the REAL product as a test instance (S-HOST 2026-10-23, ported from AudioPB's wg_real\run_real.cmd):
REM builds the helper (work\real_tools.exe) if missing, then runs real_e2e.js (needs build\x64-Debug\SuVtitle.exe +
REM SuVtitleEngine.exe + web\). Results: runs\R.jsonl, runs\R*.png, runs\console.txt. Exit 0 = every check passed.
REM Touches nothing of the user's: an isolated profile (work\profile), a test registry root (HKCU\Software\SuVtitle-Test,
REM deleted at the end), its own window class / mutex names, the engine on the CPU, the window on the second screen.
setlocal
cd /d "%~dp0"
if not exist work\real_tools.exe call build_tools.cmd || exit /b 1
if not exist runs mkdir runs
node real_e2e.js > runs\console.txt 2>&1
set "RC=%ERRORLEVEL%"
type runs\console.txt
exit /b %RC%
