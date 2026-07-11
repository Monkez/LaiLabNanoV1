@echo off
setlocal EnableExtensions
chcp 65001 >nul
cd /d "%~dp0"

echo ========================================================
echo  LaiLab Nano V1 - Development Environment Setup
echo ========================================================
echo.

set "PYTHON_CMD="
where py >nul 2>nul && set "PYTHON_CMD=py -3"
if not defined PYTHON_CMD (
    where python >nul 2>nul && set "PYTHON_CMD=python"
)
if not defined PYTHON_CMD (
    echo [ERROR] Python 3 was not found.
    echo Install Python 3.10 or newer from https://www.python.org/downloads/
    exit /b 1
)

echo [1/5] Checking Python...
%PYTHON_CMD% -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)"
if errorlevel 1 (
    echo [ERROR] Python 3.10 or newer is required.
    exit /b 1
)

echo [2/5] Preparing the web virtual environment...
if exist ".venv\Scripts\python.exe" (
    ".venv\Scripts\python.exe" -V >nul 2>nul
    if errorlevel 1 (
        echo Existing .venv is invalid and will be recreated.
        rmdir /s /q ".venv"
    )
)
if not exist ".venv\Scripts\python.exe" %PYTHON_CMD% -m venv .venv
if errorlevel 1 (
    echo [ERROR] Could not create .venv.
    exit /b 1
)

echo [3/5] Installing backend dependencies...
".venv\Scripts\python.exe" -m pip install --upgrade pip
if errorlevel 1 exit /b 1
".venv\Scripts\python.exe" -m pip install -r requirements.txt
if errorlevel 1 exit /b 1
".venv\Scripts\python.exe" -c "import flask, flask_socketio, serial, paramiko; print('Web backend dependencies: OK')"
if errorlevel 1 exit /b 1

echo [4/5] Checking Docker Desktop...
where docker >nul 2>nul || (
    echo [ERROR] Docker Desktop was not found.
    echo Install Docker Desktop, enable WSL 2, then run setup.bat again.
    exit /b 1
)
docker info >nul 2>nul || (
    echo [ERROR] Docker Desktop is installed but not running.
    echo Start Docker Desktop and run setup.bat again.
    exit /b 1
)

echo [5/5] Building the RISC-V/CVITEK development image...
docker compose -f develop\docker-compose.yml build licheerv-dev
if errorlevel 1 exit /b 1
docker compose -f develop\docker-compose.yml run --rm licheerv-dev bash /workspace/projects/OTGCamera/scripts/setup_deps.sh
if errorlevel 1 exit /b 1

echo.
echo ========================================================
echo  Setup completed successfully
echo ========================================================
echo  Web development:       start.bat
echo  Build inference:       build_inference.bat
echo  Web URL:               http://localhost:5000
echo  Inference source:      develop\Projects\OTGCamera
echo ========================================================
endlocal
