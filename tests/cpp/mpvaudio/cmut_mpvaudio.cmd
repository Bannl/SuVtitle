@echo off
REM cmut_mpvaudio.cmd [name-filter] | --dry -- the mutation sentinels of tests\cpp\mpvaudio\mpvaudio_smoke.cpp (see cmut_mpvaudio.py).
REM Needs SUVTITLE_MPV_DIR (libmpv-2.dll + include\mpv\client.h), SUVTITLE_VULKAN_RT (the loader sentinels) and the media
REM build_mpvaudio_smoke.cmd makes (work\media). Each mutant runs the whole smoke (headless: no window, no sound device).
REM One line per mutant, exit 1 if any survives.
setlocal
call "%~dp0..\..\..\tools\vsenv.cmd" || exit /b 1
call "%~dp0..\..\..\tools\pyenv.cmd" || exit /b 1
%PYRUN% "%~dp0cmut_mpvaudio.py" %*
exit /b %errorlevel%
