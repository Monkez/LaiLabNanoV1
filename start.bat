@echo off
chcp 65001 >nul
set PYTHONIOENCODING=utf-8

echo ========================================================
echo    LaiLab Nano V1 - Starting Web Server...
echo ========================================================

if not exist ".venv\Scripts\python.exe" (
    echo [ERROR] Development environment is not ready.
    echo Run setup.bat first.
    pause
    exit /b 1
)

".venv\Scripts\python.exe" app.py

pause
