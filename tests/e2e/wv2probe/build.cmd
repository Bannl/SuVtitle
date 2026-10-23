@echo off
rem ===========================================================================
rem  wv2probe build -- the WebView2 test instrument of SuVtitle's page E2E kits (NOT product code).
rem  A plain cl line, NOT the project's CMake/Ninja: the instrument must not depend on the tree it tests.
rem  (Ported 2026-10-23 from the 641f1aa-era tool of AudioPB, which SuVtitle was split out of.)
rem
rem  Needs: Visual Studio's C++ x64 tools (tools\vsenv.cmd finds them: vswhere, or SUVTITLE_VS) and the WebView2 SDK
rem  1.0.2592.51 = the Microsoft.Web.WebView2 NuGet package. Nothing of it is vendored in this repository; it is looked for
rem  in this order (the same package app\CMakeLists.txt acquires for the app):
rem    1. WEBVIEW2_NUGET_DIR = an already-extracted package (the folder that holds build\native\include and build\native\x64);
rem    2. the copy the app's configure extracted:  <repo>\build\x64-Release\_deps\webview2, then ...\x64-Debug\_deps\webview2;
rem    3. sdk-cache\webview2 next to this script (git-ignored): a download of THIS script from an earlier run;
rem    4. a download of https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2592.51 into 3 (curl.exe and
rem       tar.exe, both part of Windows 10 1803 and later).
rem  Output: wv2probe.exe here (static loader, /MT: no DLL).
rem ===========================================================================
setlocal
set "HERE=%~dp0"
set "WV2VER=1.0.2592.51"
for %%I in ("%HERE%..\..\..") do set "REPO=%%~fI"
set "PKG="
if defined WEBVIEW2_NUGET_DIR call :try "%WEBVIEW2_NUGET_DIR%"
if defined WEBVIEW2_NUGET_DIR if not defined PKG echo build: WEBVIEW2_NUGET_DIR holds no build\native\include\WebView2.h + build\native\x64\WebView2LoaderStatic.lib - ignored
if not defined PKG call :try "%REPO%\build\x64-Release\_deps\webview2"
if not defined PKG call :try "%REPO%\build\x64-Debug\_deps\webview2"
if not defined PKG call :try "%HERE%sdk-cache\webview2"
if not defined PKG call :download
if not defined PKG exit /b 1
echo build: WebView2 SDK %WV2VER% from %PKG%

call "%~dp0..\..\..\tools\vsenv.cmd" || exit /b 1
if not exist "%HERE%obj" mkdir "%HERE%obj"
cl /nologo /std:c++17 /EHsc /O2 /W3 /MT /DUNICODE /D_UNICODE ^
   /I"%PKG%\build\native\include" "%HERE%src\wv2probe.cpp" ^
   /Fo"%HERE%obj\\" /Fe"%HERE%wv2probe.exe" ^
   /link /LIBPATH:"%PKG%\build\native\x64" /SUBSYSTEM:CONSOLE
if errorlevel 1 (echo build: FAILED & exit /b 1)
echo build: OK -^> %HERE%wv2probe.exe
exit /b 0

:try
rem  call :try <folder> : PKG = <folder> when it is an extracted WebView2 package (the header + the static loader)
if exist "%~1\build\native\include\WebView2.h" if exist "%~1\build\native\x64\WebView2LoaderStatic.lib" set "PKG=%~1"
exit /b 0

:download
echo build: no WebView2 SDK %WV2VER% found locally - downloading the NuGet package from nuget.org ...
where curl >nul 2>&1 || echo build: curl.exe not found - download the package by hand and set WEBVIEW2_NUGET_DIR
where curl >nul 2>&1 || exit /b 1
where tar >nul 2>&1 || echo build: tar.exe not found - download and extract the package by hand and set WEBVIEW2_NUGET_DIR
where tar >nul 2>&1 || exit /b 1
if not exist "%HERE%sdk-cache" mkdir "%HERE%sdk-cache"
if exist "%HERE%sdk-cache\webview2.zip" del "%HERE%sdk-cache\webview2.zip"
curl -L --fail --silent --show-error -o "%HERE%sdk-cache\webview2.zip" "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/%WV2VER%"
if errorlevel 1 echo build: the download failed - fetch Microsoft.Web.WebView2 %WV2VER% by hand and set WEBVIEW2_NUGET_DIR
if errorlevel 1 exit /b 1
if not exist "%HERE%sdk-cache\webview2" mkdir "%HERE%sdk-cache\webview2"
tar -xf "%HERE%sdk-cache\webview2.zip" -C "%HERE%sdk-cache\webview2"
if errorlevel 1 echo build: the package could not be extracted
if errorlevel 1 exit /b 1
del "%HERE%sdk-cache\webview2.zip"
call :try "%HERE%sdk-cache\webview2"
if not defined PKG echo build: the package has no build\native\include\WebView2.h + build\native\x64\WebView2LoaderStatic.lib
exit /b 0
