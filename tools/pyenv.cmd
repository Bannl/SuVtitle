@echo off
rem ===========================================================================
rem  pyenv.cmd -- finds a Python 3 for the few test tools written in it (tests\cpp: cmut.py, filedocx_check.py,
rem  fixround_pins.py -- all of them standard library only) and sets PYRUN to the command that runs it ISOLATED (-I: no user
rem  site, no PYTHON* variables, nothing imported from the current folder):
rem        call "%~dp0<relative path>\tools\pyenv.cmd" || exit /b 1
rem        %PYRUN% "%~dp0the_tool.py" args
rem  Looked for, in order: SUVTITLE_PYTHON (the full path of a python.exe) -> the "py" launcher (py -3) -> python -> python3.
rem  Each candidate must run `-c "import sys"`: the Microsoft Store stub that python.exe is when Python is NOT installed fails
rem  that probe and is skipped. errorlevel 0 = PYRUN is set; 1 = no Python 3 (a message says what to do).
rem ===========================================================================
set "PYRUN="
if defined SUVTITLE_PYTHON "%SUVTITLE_PYTHON%" -I -c "import sys" >nul 2>&1 && set "PYRUN="%SUVTITLE_PYTHON%" -I" && exit /b 0
py -3 -I -c "import sys" >nul 2>&1 && set "PYRUN=py -3 -I" && exit /b 0
python -I -c "import sys" >nul 2>&1 && set "PYRUN=python -I" && exit /b 0
python3 -I -c "import sys" >nul 2>&1 && set "PYRUN=python3 -I" && exit /b 0
echo pyenv: no Python 3 was found. Install one (python.org) or set SUVTITLE_PYTHON to the full path of a python.exe.
exit /b 1
