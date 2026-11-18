@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem Copyright (c) 2026 Robert McGough
rem
rem Removes the test build for the current Windows user: unregisters the
rem OpenXR layer and deletes %LOCALAPPDATA%\Programs\OpenDesktopVR. Asks
rem before deleting settings and logs.
setlocal
set "DEST=%LOCALAPPDATA%\Programs\OpenDesktopVR"
set "DATA=%LOCALAPPDATA%\OpenDesktopVR"

echo Open Desktop VR: uninstall
echo.
echo This removes the test build for this Windows user.
echo Close every VR game, and exit Open Desktop VR from its tray icon, first.
echo.
set "ANSWER="
set /p "ANSWER=Uninstall now? Type Y and press Enter: "
if /i not "%ANSWER%"=="Y" (
  echo Nothing was changed.
  pause
  exit /b 0
)

if exist "%DEST%\odvr-layer.exe" (
  "%DEST%\odvr-layer.exe" unregister
) else (
  echo The install folder has no odvr-layer.exe; skipping the registry step.
)

echo.
set "ANSWER="
set /p "ANSWER=Also delete settings and logs in %DATA%? Type Y and press Enter, or just Enter to keep them: "
if /i "%ANSWER%"=="Y" (
  rd /s /q "%DATA%" 2>nul
  echo Settings and logs deleted.
) else (
  echo Settings and logs kept.
)

echo.
echo Deleting %DEST% ...
cd /d "%TEMP%"
rem Leave this script before deleting the folder it may be running from
(goto) 2>nul & rd /s /q "%DEST%" 2>nul & echo UNINSTALL DONE. & pause
