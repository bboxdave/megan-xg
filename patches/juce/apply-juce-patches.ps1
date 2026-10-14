# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors

<#
.SYNOPSIS
    Re-applies the MU100 guarded patches to the shared JUCE tree (libs/JUCE).

.DESCRIPTION
    The MU100 plugin makes a few small, macro-guarded edits to the shared JUCE source
    tree (see README.md). A JUCE upgrade replaces the whole tree and wipes them, so
    after any upgrade run this script to re-apply them.

    Each patch is idempotent: if the guard macro is already present in the target
    file, that patch is skipped. The guard macros are defined ONLY in the MU100
    .jucer, so applying them never affects SQ-1 / MU128 builds that share libs/JUCE.

.PARAMETER JucePath
    Path to the JUCE tree to patch. Defaults to ..\..\..\libs\JUCE relative to this
    script, which is where BUILDING.md lays it out.

.PARAMETER Verify
    Only report whether each patch is currently applied; make no changes.

.EXAMPLE
    powershell -File patches\juce\apply-juce-patches.ps1
    powershell -File patches\juce\apply-juce-patches.ps1 -Verify
#>
param(
    [string]$JucePath = (Join-Path $PSScriptRoot "..\..\..\libs\JUCE"),
    [switch]$Verify
)

$ErrorActionPreference = "Stop"

# patch file (in this dir)  ->  target file (relative to JUCE root)  ->  guard macro token
$patches = @(
    @{ Patch = "juce_audio_plugin_client_VST3.cpp.patch"
       Target = "modules\juce_audio_plugin_client\juce_audio_plugin_client_VST3.cpp"
       Macro  = "JucePlugin_MU100_DualMidiIn" },
    @{ Patch = "juce_StandaloneFilterWindow.h.patch"
       Target = "modules\juce_audio_plugin_client\Standalone\juce_StandaloneFilterWindow.h"
       Macro  = "JucePlugin_MU100_HideStandaloneOptions" }
)

$juce = (Resolve-Path $JucePath).Path
Write-Host "JUCE tree: $juce`n"

$problems = 0
foreach ($p in $patches) {
    $patchPath  = Join-Path $PSScriptRoot $p.Patch
    $targetPath = Join-Path $juce $p.Target
    $name = $p.Patch

    if (-not (Test-Path $targetPath)) {
        Write-Host "[MISS] $name : target not found ($($p.Target))" -ForegroundColor Red
        $problems++; continue
    }

    $applied = Select-String -Path $targetPath -Pattern $p.Macro -SimpleMatch -Quiet

    if ($Verify) {
        if ($applied) { Write-Host "[ OK ] $name : applied"        -ForegroundColor Green }
        else          { Write-Host "[MISS] $name : NOT applied"     -ForegroundColor Yellow }
        continue
    }

    if ($applied) {
        Write-Host "[SKIP] $name : already applied" -ForegroundColor DarkGray
        continue
    }

    # git apply works on plain files even outside a git repo, but the target paths
    # must be relative to the current directory, so run it from inside the JUCE tree.
    #
    # --ignore-whitespace is REQUIRED, not optional tidiness. The patch files are
    # stored LF (see .gitattributes) while the JUCE tree on this machine is CRLF, so
    # every context line differs by a trailing CR and a plain `git apply` rejects the
    # patch outright ("patch does not apply" at the first hunk). Verified 2026-08-04:
    # without this flag NEITHER patch applies to a CRLF tree; with it both do.
    # Keeping the patches LF (rather than storing them CRLF) is deliberate — it is the
    # combination that works whether the upgraded tree arrives LF or CRLF.
    Push-Location $juce
    try   { & git apply -p1 --ignore-whitespace --directory=. "$patchPath" }
    finally { Pop-Location }
    if ($LASTEXITCODE -eq 0) {
        Write-Host "[ OK ] $name : applied" -ForegroundColor Green
    } else {
        Write-Host "[FAIL] $name : git apply rejected the patch." -ForegroundColor Red
        Write-Host "        JUCE may have moved the surrounding code. Re-apply by hand:" -ForegroundColor Red
        Write-Host "        open $($p.Patch), and insert each hunk at the matching anchor" -ForegroundColor Red
        Write-Host "        (grep the target for '$($p.Macro)' to confirm)." -ForegroundColor Red
        $problems++
    }
}

if ($problems -gt 0) { Write-Host "`n$problems problem(s)." -ForegroundColor Red; exit 1 }
else { Write-Host "`nDone." -ForegroundColor Green }
