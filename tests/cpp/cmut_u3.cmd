@echo off
REM cmut_u3.cmd [app_u3|player_u3|docx_u3|tc_u3|trmgr_u3|all] [name-filter] | --dry -- the C++ mutation sentinels of U3 (the
REM TRANSCRIPTION, see cmut_u3.py). trmgr_u3 needs tests\cpp\work\trmgr\SuVtitleEngine.exe (build_trmgr_smoke.cmd first).
REM Results: one line per mutant, exit 1 if any survives.
setlocal
call "%~dp0..\..\tools\vsenv.cmd" || exit /b 1
call "%~dp0..\..\tools\pyenv.cmd" || exit /b 1
%PYRUN% "%~dp0cmut_u3.py" %*
exit /b %errorlevel%
