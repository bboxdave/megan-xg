# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors

# Keyboard helpers for ShotKit. Every key is guarded on the plugin window ACTUALLY
# being in the foreground: SetForegroundWindow can fail silently, and an unguarded
# keystroke then goes to whatever app is really in front.
#
# THE SCAN CODE IS NOT OPTIONAL. JUCE's Windows peer resolves a WM_CHAR back to the
# key's UNSHIFTED character by mapping the message's SCAN CODE (juce_Windowing_windows
# doKeyChar), so a keybd_event sent with scan code 0 makes MapVirtualKey return 0 and
# JUCE keeps the shifted character instead -- Shift+. arrives as '>' rather than as
# 'shift + .'. That is an artefact of the harness, not of the plugin, and it will
# produce a FALSE NEGATIVE for any punctuation binding. Always send the real scan code.
if (-not ([System.Management.Automation.PSTypeName]'KeyScan').Type) {
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class KeyScan {
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint uCode, uint uMapType);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, UIntPtr e);
    public static void Tap(byte vk, bool down) {
        byte sc = (byte) MapVirtualKey(vk, 0);        // MAPVK_VK_TO_VSC
        keybd_event(vk, sc, down ? 0u : 2u, UIntPtr.Zero);
    }
}
'@
}
function Shot-Key {
    param([int]$Vk, [switch]$Shift, [int]$Repeat = 1)
    for ($i = 0; $i -lt $Repeat; $i++) {
        if ([Shot]::GetForegroundWindow() -ne $script:ShotHwnd) {
            if (-not (Shot-Front)) { Write-Warning "KEY REFUSED - plugin is not in front"; return $false }
        }
        if ($Shift) { [KeyScan]::Tap(0x10, $true) }
        [KeyScan]::Tap([byte]$Vk, $true)
        Start-Sleep -Milliseconds 40
        [KeyScan]::Tap([byte]$Vk, $false)
        if ($Shift) { [KeyScan]::Tap(0x10, $false) }
        Start-Sleep -Milliseconds 220
    }
    return $true
}
function Shot-KeyAlt { param([int]$Vk)
    if ([Shot]::GetForegroundWindow() -ne $script:ShotHwnd) {
        if (-not (Shot-Front)) { Write-Warning "KEY REFUSED"; return $false }
    }
    [KeyScan]::Tap(0x12, $true)
    [KeyScan]::Tap([byte]$Vk, $true)
    Start-Sleep -Milliseconds 40
    [KeyScan]::Tap([byte]$Vk, $false)
    [KeyScan]::Tap(0x12, $false)
    Start-Sleep -Milliseconds 400
    return $true
}
$VK = @{ TAB=0x09; RET=0x0D; ESC=0x1B; LEFT=0x25; UP=0x26; RIGHT=0x27; DOWN=0x28; BACK=0x08; V=0x56; COMMA=0xBC; PERIOD=0xBE }
function Snap { param([string]$n)
    $dir = Join-Path ([IO.Path]::GetTempPath()) "megan-xg-shots"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $path = Join-Path $dir $n
    [Shot]::Capture($script:ShotHwnd, $path, 0,0,0,0)
    "$n  $([Shot]::Describe($path))"
}
