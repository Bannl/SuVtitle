@echo off
rem ===========================================================================
rem  vsenv.cmd -- the ONE place that finds Visual Studio and loads its x64 C++ environment. Every script of this tree that
rem  needs cl / link / rc / cmake / ninja starts with
rem        call "%~dp0<relative path>\tools\vsenv.cmd" || exit /b 1
rem  right after its own `setlocal` (what this file sets stays in the caller's scope, nothing leaks into your shell).
rem
rem  Where Visual Studio comes from, in order:
rem    1. the environment variable SUVTITLE_VS = an install ROOT, the folder that holds VC\Auxiliary\Build\vcvars64.bat
rem       (for an install vswhere does not list, or to force one of several);
rem    2. an environment that is ALREADY loaded (a "Developer Command Prompt": VSINSTALLDIR is set and cl is on PATH) is
rem       reused as it is, unless SUVTITLE_VS names another install;
rem    3. vswhere, which every Visual Studio ships at
rem       %ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe: the latest install that has the
rem       "C++ x64/x86 build tools" component -- the released channels first, then the preview / insiders ones
rem       (vswhere hides those without -prerelease; the author's VS 2026 is one of them).
rem
rem  Sets: SUVTITLE_VS (the install root in use), everything vcvars64.bat sets (cl, link, rc ... on PATH), and
rem        CMAKE / NINJA = the full paths of the cmake.exe / ninja.exe bundled with that Visual Studio, else the first ones
rem        on PATH, else empty.
rem  errorlevel 0 = ready; 1 = nothing usable (a message says what to do).
rem ===========================================================================
rem (%%~fI normalises a path: no trailing backslash. Substring expansions are avoided on purpose: on an UNDEFINED variable
rem  cmd mangles the whole line.)
set "_vsroot="
set "_vscur="
set "_vsreq="
if defined VSINSTALLDIR for %%I in ("%VSINSTALLDIR%.") do set "_vscur=%%~fI"
if defined SUVTITLE_VS for %%I in ("%SUVTITLE_VS%\.") do set "_vsreq=%%~fI"
if defined _vscur if defined _vsreq if /i not "%_vsreq%"=="%_vscur%" goto :pick
if defined _vscur where cl >nul 2>&1 && set "_vsroot=%_vscur%" && goto :tools

:pick
if defined _vsreq set "_vsroot=%_vsreq%"
if not defined _vsroot call :vswhere
if not defined _vsroot goto :novs
if not exist "%_vsroot%\VC\Auxiliary\Build\vcvars64.bat" goto :badroot
call "%_vsroot%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where cl >nul 2>&1 || goto :nocl

:tools
set "CMAKE="
set "NINJA="
if exist "%_vsroot%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE=%_vsroot%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if exist "%_vsroot%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" set "NINJA=%_vsroot%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not defined CMAKE for /f "delims=" %%I in ('where cmake 2^>nul') do if not defined CMAKE set "CMAKE=%%I"
if not defined NINJA for /f "delims=" %%I in ('where ninja 2^>nul') do if not defined NINJA set "NINJA=%%I"
set "SUVTITLE_VS=%_vsroot%"
set "_vsroot="
set "_vscur="
set "_vsreq="
set "_vsw="
exit /b 0

:vswhere
set "_vsw=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%_vsw%" set "_vsw=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%_vsw%" exit /b 0
for /f "usebackq delims=" %%I in (`"%_vsw%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do if not defined _vsroot set "_vsroot=%%I"
if defined _vsroot exit /b 0
for /f "usebackq delims=" %%I in (`"%_vsw%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do if not defined _vsroot set "_vsroot=%%I"
exit /b 0

:novs
echo vsenv: no Visual Studio with the C++ x64 build tools was found.
echo vsenv: install Visual Studio or its Build Tools with the "Desktop development with C++" workload,
echo vsenv: or set SUVTITLE_VS to the install root, the folder that holds VC\Auxiliary\Build\vcvars64.bat.
goto :fail

:badroot
echo vsenv: "%_vsroot%" holds no VC\Auxiliary\Build\vcvars64.bat.
echo vsenv: SUVTITLE_VS must be an install root (the folder that has VC\ and Common7\ in it).
goto :fail

:nocl
echo vsenv: vcvars64.bat ran but cl.exe is not on PATH: the C++ x64 tools are not installed in "%_vsroot%".
goto :fail

:fail
set "_vsroot="
set "_vscur="
set "_vsreq="
set "_vsw="
exit /b 1
