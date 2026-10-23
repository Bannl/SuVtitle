@echo off
rem buildlock.cmd <config> [cmake --build extra args...]   -- SuVtitle's ONLY way to build.
rem   <config> = x64-Debug | x64-Release      e.g.  tools\buildlock.cmd x64-Debug --target suvtitle_engine
rem Configures build\<config> on first use (Ninja + MSVC x64), then builds. The Visual Studio, its cmake and its ninja are
rem found by tools\vsenv.cmd (vswhere; SUVTITLE_VS names an install root when vswhere cannot).
rem The first configure also needs the WebView2 SDK (a NuGet package, see app\CMakeLists.txt): downloaded unless
rem WEBVIEW2_NUGET_DIR names an extracted copy.
rem Serialises every build of this tree behind ONE lock: two Ninja processes in the same build dir
rem corrupt .ninja_log/.ninja_deps. A lock older than ~30 min is assumed stale and broken.
setlocal
set "ROOT=%~dp0.."
set "LOCK=%~dp0build.lock"
set "CFG=%~1"
if "%CFG%"=="" set "CFG=x64-Debug"
set "BT=Debug"
if /i "%CFG%"=="x64-Release" set "BT=Release"
set /a tries=0
:retry
mkdir "%LOCK%" 2>nul && goto locked
set /a tries+=1
if %tries% GEQ 360 (
  echo [buildlock] lock held for 30 min - breaking it
  rmdir "%LOCK%" 2>nul
  set /a tries=0
  goto retry
)
if %tries%==1 echo [buildlock] waiting for another build to finish...
ping -n 6 127.0.0.1 >nul
goto retry
:locked
call "%~dp0vsenv.cmd"
if errorlevel 1 goto done
if not defined CMAKE goto nocmake
if exist "%ROOT%\build\%CFG%\CMakeCache.txt" goto build
if not defined NINJA goto noninja
"%CMAKE%" -S "%ROOT%" -B "%ROOT%\build\%CFG%" -G Ninja -DCMAKE_BUILD_TYPE=%BT% -DCMAKE_MAKE_PROGRAM="%NINJA%"
if errorlevel 1 goto done
:build
"%CMAKE%" --build "%ROOT%\build\%CFG%" %2 %3 %4 %5 %6
:done
set RC=%errorlevel%
rmdir "%LOCK%" 2>nul
exit /b %RC%
:nocmake
echo [buildlock] no cmake.exe: install Visual Studio's "C++ CMake tools" component, or put cmake on PATH
rmdir "%LOCK%" 2>nul
exit /b 1
:noninja
echo [buildlock] no ninja.exe: install Visual Studio's "C++ CMake tools" component, or put ninja on PATH
rmdir "%LOCK%" 2>nul
exit /b 1
