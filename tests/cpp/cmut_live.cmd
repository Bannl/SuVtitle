@echo off
REM cmut_live.cmd [trmgr_live|player_live|app_live|all] [name-filter] | --dry -- the C++ mutation sentinels of the LIVE parts (see cmut_live.py).
REM trmgr_live needs work\trmgr\SuVtitleEngine.exe: run build_trmgr_smoke.cmd first. Results: one line per mutant, exit 1 if any survives.
setlocal
call "%~dp0..\..\tools\vsenv.cmd" || exit /b 1
call "%~dp0..\..\tools\pyenv.cmd" || exit /b 1
%PYRUN% "%~dp0cmut_live.py" %*
exit /b %errorlevel%
