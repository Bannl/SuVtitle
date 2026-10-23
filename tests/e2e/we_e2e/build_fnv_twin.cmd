@echo off
rem ===========================================================================
rem  build_fnv_twin.cmd -- builds fnv_twin.exe (the product's folder/media keys and host URLs, printed by app\TrPlayer.h's own
rem  code) for check_hosts.js. T-E2E, 2026-10-23 (RV4-12). Plain cl, /W4, no /utf-8 (like the product), VS 2026 vcvars64.
rem  Output: %SUVTITLE_T_E2E_WORK% if set, else <repo>\scratchpad\t-e2e\fnv_twin\ (git-ignored; NEVER build\). The exe's path
rem  is the last line printed: "fnv_twin: OK -> <exe>". "vswhere is not recognized" printed by vcvars is noise.
rem ===========================================================================
setlocal
set "HERE=%~dp0"
if defined SUVTITLE_T_E2E_WORK ( set "OUT=%SUVTITLE_T_E2E_WORK%" ) else ( for %%I in ("%~dp0..\..\..\scratchpad\t-e2e\fnv_twin") do set "OUT=%%~fI" )
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
call "%~dp0..\..\..\tools\vsenv.cmd" || exit /b 1
where cl >nul 2>&1 || (echo fnv_twin: cl not found after vcvars64 & exit /b 1)
cl /nologo /std:c++17 /EHsc /W4 /MD /O2 ^
   /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I"%HERE%..\..\..\common" ^
   "%HERE%fnv_twin.cpp" /Fe:"%OUT%\fnv_twin.exe" /Fo:"%OUT%\obj\\" /link user32.lib shlwapi.lib ole32.lib
if errorlevel 1 (echo fnv_twin: BUILD FAILED & exit /b 1)
echo fnv_twin: OK -^> %OUT%\fnv_twin.exe
exit /b 0
