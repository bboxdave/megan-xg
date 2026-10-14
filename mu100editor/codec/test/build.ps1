# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors

# Build + run the MU100 SysEx codec tests with MSVC (cl.exe).
# Usage:  powershell -ExecutionPolicy Bypass -File codec/test/build.ps1
$ErrorActionPreference = "Stop"
$here   = Split-Path -Parent $MyInvocation.MyCommand.Path          # codec/test
$codec  = Split-Path -Parent $here                                  # codec
$root   = Split-Path -Parent $codec                                 # mu100editor
$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$out = Join-Path $here "sysex_test.exe"
$src = @("$here\sysex_test.cpp", "$codec\MU100SysEx.cpp", "$codec\MU100State.cpp", "$codec\MU100AddressMap.cpp") -join " "
# Compile via a child cmd that first runs vcvars so cl.exe is on PATH.
$cmd = "`"$vcvars`" >nul && cl /nologo /EHsc /std:c++17 /W4 /Fe:`"$out`" /Fo:`"$here\\`" $src"
Write-Host "Compiling..." -ForegroundColor Cyan
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }

Write-Host "`nRunning..." -ForegroundColor Cyan
$fixtures = Join-Path $root "reference\syx examples"
& $out "$fixtures"
exit $LASTEXITCODE
