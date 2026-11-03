@echo off
rem capmut.cmd [name-filter | --check] -- the mutation sentinels of the LIVE capture hub (wave L1b tests, 2026-10-25): ~20
rem one-line changes to the hub's sources, each of which capture_smoke --quick (virtual clock, no device) must fail on.
rem Reuses tests\cpp\cmut.py's machinery through capmut.py. Scratch tree: SUVTITLE_CMUT_WORK, else %%TEMP%%\suvtitle_cmut
rem (deleted/recreated per mutant); the repository files are never touched. About 40 s per mutant. C2 reads
rem SUVTITLE_TEST_MEDIA when it is set (bench_en.wav / bench_zh.wav).
setlocal
call "%~dp0..\..\..\tools\vsenv.cmd" || exit /b 1
call "%~dp0..\..\..\tools\pyenv.cmd" || exit /b 1
%PYRUN% "%~dp0capmut.py" %*
exit /b %errorlevel%
