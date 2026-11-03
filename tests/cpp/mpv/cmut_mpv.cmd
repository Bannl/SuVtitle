@echo off
REM cmut_mpv.cmd [name-filter] | --dry -- the mutation sentinels of tests\cpp\mpv\mpv_smoke.cpp (see cmut_mpv.py). Needs SUVTITLE_MPV_DIR
REM (libmpv-2.dll + include\mpv\client.h) and the media build_mpv_smoke.cmd makes (work\media). About 35 minutes for all (41 mutants, measured 02/11): each mutant
REM runs the whole smoke (its windows on the second screen, never activated, mpv silent). One line per mutant, exit 1 if any survives.
setlocal
call "%~dp0..\..\..\tools\vsenv.cmd" || exit /b 1
call "%~dp0..\..\..\tools\pyenv.cmd" || exit /b 1
%PYRUN% "%~dp0cmut_mpv.py" %*
exit /b %errorlevel%
