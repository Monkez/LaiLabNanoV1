@echo off
setlocal EnableExtensions
chcp 65001 >nul
cd /d "%~dp0"

where docker >nul 2>nul || (
    echo [ERROR] Docker was not found. Run setup.bat first.
    exit /b 1
)

docker info >nul 2>nul || (
    echo [ERROR] Docker Desktop is not running.
    exit /b 1
)

echo Building Yolo_CSIStream and reset_btn...
docker compose -f develop\docker-compose.yml run --rm licheerv-dev bash /workspace/projects/OTGCamera/scripts/build.sh
if errorlevel 1 exit /b 1

echo.
echo Binaries are available in develop\Projects\OTGCamera\build
endlocal
