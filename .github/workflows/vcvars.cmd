@echo off
rem Copyright (c) 2026 Jakub Melka and Contributors
rem SPDX-License-Identifier: MIT
rem See LICENSE file in the project root for full license text.
rem
rem Locates the newest Visual Studio installation carrying the MSVC C++
rem toolchain and enters its x64 developer environment, mirroring what
rem WinBuild.ps1 does locally. The Ninja presets use
rem "architecture": { "strategy": "external" }, so the environment must be
rem prepared before CMake runs.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe was not found at "%VSWHERE%".
    exit /b 1
)

set "VSINSTALL="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"

if not defined VSINSTALL (
    echo No Visual Studio installation with the MSVC C++ toolchain was found.
    exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
