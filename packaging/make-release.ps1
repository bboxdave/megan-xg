# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors

<#
.SYNOPSIS
    Builds the MEGAN XG release zip: VST3 + standalone + manual + notices.

.DESCRIPTION
    The zip mirrors the install locations, so each top-level folder names where
    its contents go (see packaging\INSTALL.txt):

        INSTALL.txt, LICENSE.txt, THIRD-PARTY-NOTICES.txt
        Common Files\VST3\MEGAN XG.vst3\
        Documents\MEGAN XG\manual\MEGAN-XG-Manual.html
        Standalone\MEGAN XG.exe

    Steps: full /t:Rebuild (the About box's build date only refreshes when
    PluginEditor.cpp recompiles), single-file manual + its checker, the notices
    from the SAME MAME and JUCE trees the binary links, then the zip and its
    SHA-256. Run it from PowerShell, never Git Bash (MSBuild switches).

.PARAMETER SkipBuild
    Package the binaries already in Builds\ instead of rebuilding.
#>
param(
    [string] $MamePath = (Join-Path $PSScriptRoot '..\..\libs\mame-mu100'),
    [string] $JucePath = (Join-Path $PSScriptRoot '..\..\libs\JUCE'),
    [string] $OutDir   = (Join-Path $PSScriptRoot '..\dist'),
    [switch] $SkipBuild
)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..')
$MamePath = Resolve-Path $MamePath
$JucePath = Resolve-Path $JucePath
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path   # the listing at the end trims this prefix
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'

function Step($text) { Write-Host "== $text" -ForegroundColor Cyan }
function Check($what) { if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" } }

# The project version, from the JUCERPROJECT element (the first version= in the
# file is the XML declaration's).
$jucer = Get-Content (Join-Path $repo 'MEGAN XG.jucer') -Raw
$m = [regex]::Match($jucer, '<JUCERPROJECT\b[^>]*?\sversion="([^"]+)"')
if (-not $m.Success) { throw 'no version attribute on JUCERPROJECT in MEGAN XG.jucer' }
$version = $m.Groups[1].Value
$name = "MEGAN-XG-$version-win64"
Step "packaging $name"

$rel = Join-Path $repo 'Builds\VisualStudio2026\x64\Release'
if (-not $SkipBuild) {
    if (Get-Process 'MEGAN XG' -ErrorAction SilentlyContinue) {
        throw 'MEGAN XG.exe is running; close it first (the link would fail with LNK1104)'
    }
    Step 'full rebuild'
    & $msbuild (Join-Path $repo 'Builds\VisualStudio2026\MEGAN XG.sln') /t:Rebuild `
        /p:Configuration=Release /p:Platform=x64 /m /v:minimal
    Check 'MSBuild'
}
$vst3 = Join-Path $rel 'VST3\MEGAN XG.vst3'
$exe  = Join-Path $rel 'Standalone Plugin\MEGAN XG.exe'
foreach ($p in $vst3, $exe) { if (-not (Test-Path $p)) { throw "missing build output: $p" } }

Step 'manual (single file)'
$manualDir = Join-Path $repo 'docs\manual'
$manual = Join-Path $manualDir 'MEGAN-XG-Manual.html'
Push-Location $manualDir
try {
    python build_manual.py --inline-images; Check 'build_manual.py'
    python tools\check_manual.py;           Check 'check_manual.py'
} finally { Pop-Location }

Step 'staging'
$stage = Join-Path $OutDir $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$vstDest = Join-Path $stage 'Common Files\VST3'
$docDest = Join-Path $stage 'Documents\MEGAN XG\manual'
$exeDest = Join-Path $stage 'Standalone'
New-Item -ItemType Directory -Force $vstDest, $docDest, $exeDest | Out-Null
Copy-Item -Recurse $vst3 $vstDest
Copy-Item $manual $docDest
Copy-Item $exe $exeDest
Copy-Item (Join-Path $repo 'LICENSE') (Join-Path $stage 'LICENSE.txt')
(Get-Content (Join-Path $PSScriptRoot 'INSTALL.txt') -Raw).Replace('{VERSION}', $version) |
    Set-Content (Join-Path $stage 'INSTALL.txt') -Encoding utf8 -NoNewline

Step 'third-party notices'
python (Join-Path $PSScriptRoot 'gen_notices.py') --mame $MamePath --juce $JucePath `
    --out (Join-Path $stage 'THIRD-PARTY-NOTICES.txt')
Check 'gen_notices.py'

Step 'zip'
$zip = Join-Path $OutDir "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
# Not Compress-Archive: Windows PowerShell 5.1's stores entry names with
# BACKSLASHES, against the ZIP spec (APPNOTE 4.4.17), and non-Windows unzippers
# then extract "Common Files\VST3\..." as one flat filename.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, 'Create')
try {
    Get-ChildItem -Recurse -File $stage | Sort-Object FullName | ForEach-Object {
        $entry = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive, $_.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $archive.Dispose() }
$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
"$hash  $name.zip" | Set-Content "$zip.sha256" -Encoding ascii

Get-ChildItem -Recurse -File $stage | ForEach-Object {
    '{0,12:N0}  {1}' -f $_.Length, $_.FullName.Substring($stage.Length + 1)
}
Write-Host ''
Write-Host "$zip" -ForegroundColor Green
Write-Host "sha256 $hash"
