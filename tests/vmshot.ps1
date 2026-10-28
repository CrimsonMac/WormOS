# Saves a PNG of the VMware Workstation window whose title starts with "WormOS".
# Usage: powershell.exe -NoProfile -ExecutionPolicy Bypass -File D:/wormos-tools/vmshot.ps1 OUT.png
# Captures only that window (even when it is behind others); changes nothing.
param([Parameter(Mandatory = $true)][string]$Out)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class VmShot {
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
}
"@

[VmShot]::SetProcessDPIAware() | Out-Null
$proc = Get-Process vmware -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowTitle -like 'WormOS*' } | Select-Object -First 1
if (-not $proc) { Write-Error "no VMware window titled WormOS*"; exit 1 }

$rect = New-Object VmShot+RECT
[VmShot]::GetWindowRect($proc.MainWindowHandle, [ref]$rect) | Out-Null
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
if ($width -le 0 -or $height -le 0) { Write-Error "window is minimised"; exit 1 }

$bitmap = New-Object System.Drawing.Bitmap $width, $height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$hdc = $graphics.GetHdc()
# 2 = PW_RENDERFULLCONTENT: also captures hardware-accelerated content.
[VmShot]::PrintWindow($proc.MainWindowHandle, $hdc, 2) | Out-Null
$graphics.ReleaseHdc($hdc)
$graphics.Dispose()
$bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()
"$($proc.MainWindowTitle): ${width}x${height} -> $Out"
