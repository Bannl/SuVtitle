@echo off
REM cmut_u2.cmd [app_u2|player_u2|all] [name-filter] | --dry -- the C++ mutation sentinels of U2 ("Import model...", see cmut_u2.py).
REM Results: one line per mutant, exit 1 if any survives.
setlocal
call "%~dp0..\..\tools\vsenv.cmd" || exit /b 1
call "%~dp0..\..\tools\pyenv.cmd" || exit /b 1
%PYRUN% "%~dp0cmut_u2.py" %*
exit /b %errorlevel%
