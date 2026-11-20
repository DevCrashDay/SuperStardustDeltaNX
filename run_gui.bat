@echo off
cd /d "%~dp0"
where py >nul 2>&1 && (py -3 gui.py & exit /b %errorlevel%)
python gui.py
