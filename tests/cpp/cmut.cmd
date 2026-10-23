@echo off
REM cmut.cmd [subwriter|filestore|trmgr|pipe|player|playerwh|wg|tracks|subreader|all] [name-filter] -- the C++ mutation
REM sentinels (see cmut.py). Ported 2026-10-23 from AudioPB 641f1aa. The trmgr target needs
REM work\trmgr\SuVtitleEngine.exe: run build_trmgr_smoke.cmd first; the pipe target (S-HOST fix round, 2026-10-23)
REM needs work\pipe\SuVtitleEngine.exe: run build_pipe_smoke.cmd first.
setlocal
call "%~dp0..\..\tools\vsenv.cmd" || exit /b 1
call "%~dp0..\..\tools\pyenv.cmd" || exit /b 1
%PYRUN% "%~dp0cmut.py" %*
exit /b %errorlevel%
