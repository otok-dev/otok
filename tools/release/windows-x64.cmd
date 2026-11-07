@echo off
:: Otok (c) Alexander A Pototskiy
:: SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception
::
:: Runs windows-x64.ps1 (the self-contained otok of a release for Windows x64, packed for Scoop).
:: Usage: windows-x64.cmd [-Output DIR]
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0windows-x64.ps1" %*
exit /b %ERRORLEVEL%
