@echo off
REM cmut_upd.cmd [upd] [name-filter] | --dry -- the C++ mutation sentinels of THE UPDATE BANNER (see cmut_upd.py): each one-line
REM change to app\NextaleUpdate.* or AppConfig.h's update lines must make update_smoke fail. Exit 1 if any survives.
setlocal
call "%~dp0..\..\tools\vsenv.cmd" || exit /b 1
call "%~dp0..\..\tools\pyenv.cmd" || exit /b 1
%PYRUN% "%~dp0cmut_upd.py" %*
exit /b %errorlevel%
