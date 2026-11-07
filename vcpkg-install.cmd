@echo off
:: Otok (c) Alexander A Pototskiy
:: SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-otok-output-exception
::
:: Installs the dependencies of vcpkg.json into vcpkg_installed\ (manifest mode).
:: Expects a vcpkg checkout next to this repository (..\vcpkg), or VCPKG_ROOT pointing at one.
setlocal
set "ROOT=%~dp0"
if "%VCPKG_ROOT%"=="" set "VCPKG_ROOT=%ROOT%..\vcpkg"
"%VCPKG_ROOT%\vcpkg.exe" install --x-manifest-root="%ROOT%." --x-install-root="%ROOT%vcpkg_installed" --overlay-triplets="%ROOT%triplets" --triplet=x64-windows-otok %*
