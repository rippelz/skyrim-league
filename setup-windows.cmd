@echo off
cd /d "%~dp0"
py -3 -m pip install -r requirements-windows.txt
if errorlevel 1 goto failed
py -3 tools\install_windows.py %*
if errorlevel 1 goto failed
exit /b 0
:failed
pause
exit /b 1
