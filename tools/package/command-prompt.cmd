@echo off
rem SPDX-License-Identifier: LicenseRef-Proprietary
rem Copyright (c) 2026 Robert McGough. All rights reserved.
rem
rem Opens a Command Prompt in this folder, ready for odvr-layer commands.
cd /d "%~dp0"
cmd /k "echo Open Desktop VR command prompt, in %~dp0& echo Type odvr-layer and press Enter to see the commands.& echo."
