@echo off
setlocal
set "ACAH_VR_SCRIPT=%~dp0tools\Launch-VR.ps1"
if not exist "%ACAH_VR_SCRIPT%" set "ACAH_VR_SCRIPT=%~dp0vr-poc\tools\Launch-VR.ps1"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%ACAH_VR_SCRIPT%" %*
if errorlevel 1 (
    echo.
    echo VR launcher failed. Read the error above.
    pause
    exit /b 1
)
exit /b 0
