@echo off
cd /d "%~dp0"
py -3 tools\manager.py --open %*
if errorlevel 1 pause
