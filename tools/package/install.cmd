@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem Copyright (c) 2026 Robert McGough
rem
rem Installs this test build for the current Windows user, with no admin
rem rights (ADR-009): copies it to %LOCALAPPDATA%\Programs\OpenDesktopVR,
rem registers the OpenXR layer and turns the layer's test card off. Running it again
rem from a newer package updates the installation.
setlocal
set "SRC=%~dp0"
set "DEST=%LOCALAPPDATA%\Programs\OpenDesktopVR"
set "LOGDIR=%LOCALAPPDATA%\OpenDesktopVR\logs"
set "REPORT=%LOGDIR%\install.txt"
if not exist "%LOGDIR%" mkdir "%LOGDIR%"

call :install > "%REPORT%" 2>&1
set "RESULT=%ERRORLEVEL%"
type "%REPORT%"
echo.
echo (This report is saved in %REPORT%)
echo.
pause
exit /b %RESULT%

:install
echo Open Desktop VR: installing the test build
echo.
type "%SRC%BUILD-INFO.txt"
echo.

if /i "%SRC%"=="%DEST%\" (
  echo Already running from the install folder: nothing to copy.
) else (
  echo Copying to %DEST% ...
  robocopy "%SRC%." "%DEST%" /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP >nul
  if errorlevel 8 (
    echo.
    echo INSTALL FAILED: some files couldn't be copied.
    echo Close every VR game, and exit Open Desktop VR from its tray icon,
    echo then run install again.
    exit /b 1
  )
  echo Copied.
)
echo.

echo Registering the OpenXR layer for this user ...
"%DEST%\odvr-layer.exe" register
if errorlevel 1 (
  echo.
  echo INSTALL FAILED: the layer couldn't be registered.
  exit /b 1
)
echo.

echo Turning the layer's own test card off (the host app's panels replace it) ...
"%DEST%\odvr-layer.exe" test-panel off
echo.

echo OpenXR layers registered on this PC:
"%DEST%\odvr-layer.exe" list
echo.
echo INSTALL DONE.
exit /b 0
