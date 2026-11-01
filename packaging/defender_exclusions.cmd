@echo off

setlocal
set "TARGET=%~dp0"
if "%TARGET:~-1%"=="\" set "TARGET=%TARGET:~0,-1%"

net session >nul 2>&1
if errorlevel 1 (
    echo.
    echo   This needs to run as Administrator.
    echo   Right-click the file and choose "Run as administrator".
    echo.
    pause
    exit /b 1
)

if /I "%~1"=="remove" goto :remove

echo.
echo   Adding a Defender exclusion for:
echo       %TARGET%
echo.
powershell -NoProfile -Command "Add-MpPreference -ExclusionPath '%TARGET%'"
if errorlevel 1 goto :failed
powershell -NoProfile -Command "Add-MpPreference -ExclusionProcess 'qoh99caster.exe'"
powershell -NoProfile -Command "Add-MpPreference -ExclusionProcess 'qoh99_netplay.dll'"
powershell -NoProfile -Command "Add-MpPreference -ExclusionProcess 'ddraw.dll'"
echo.
echo   Done. If a file was already quarantined, restore it from
echo   Windows Security ^> Virus ^& threat protection ^> Protection history.
echo.
pause
exit /b 0

:remove
echo.
echo   Removing the Defender exclusion for:
echo       %TARGET%
echo.
powershell -NoProfile -Command "Remove-MpPreference -ExclusionPath '%TARGET%'"
powershell -NoProfile -Command "Remove-MpPreference -ExclusionProcess 'qoh99caster.exe'"
powershell -NoProfile -Command "Remove-MpPreference -ExclusionProcess 'qoh99_netplay.dll'"
powershell -NoProfile -Command "Remove-MpPreference -ExclusionProcess 'ddraw.dll'"
echo.
echo   Removed.
echo.
pause
exit /b 0

:failed
echo.
echo   Failed. Defender may be managed by policy, or Tamper Protection may be
echo   blocking the change. You can add the folder by hand:
echo     Windows Security ^> Virus ^& threat protection ^> Manage settings ^>
echo     Exclusions ^> Add an exclusion ^> Folder
echo.
pause
exit /b 1
