[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $project 'dist\SM2DebugMenuPreview.exe'
$verification = Join-Path $project 'verification'
$null = New-Item -ItemType Directory -Path $verification -Force

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class MenuSmokeWin32 {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr hwnd, int id);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int command);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out Rect rect);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
}
'@
function Assert-Condition([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Select-Page([IntPtr]$Window, [int]$Id) {
    $null = [MenuSmokeWin32]::SendMessage($Window, 0x111, [IntPtr]$Id, [IntPtr]::Zero)
}
function Save-MenuImage([IntPtr]$Window, [string]$Name) {
    $rect = New-Object MenuSmokeWin32+Rect
    $null = [MenuSmokeWin32]::GetWindowRect($Window, [ref]$rect)
    $bitmap = New-Object Drawing.Bitmap(($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top))
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    try { $printed = [MenuSmokeWin32]::PrintWindow($Window, $dc, 2) }
    finally { $graphics.ReleaseHdc($dc); $graphics.Dispose() }
    try {
        Assert-Condition $printed 'PrintWindow failed for the preview.'
        $bitmap.Save((Join-Path $verification $Name), [Drawing.Imaging.ImageFormat]::Png)
    } finally { $bitmap.Dispose() }
}

$previewProcess = Start-Process -FilePath $executable -WorkingDirectory $project -WindowStyle Hidden -PassThru
try {
    $window = [IntPtr]::Zero
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ([DateTime]::UtcNow -lt $deadline) {
        $candidate = [MenuSmokeWin32]::FindWindow('SM2DebugMenuWindow', 'SM2 Debug Menu - PREVIEW (no game bindings)')
        $owner = [uint32]0
        if ($candidate -ne [IntPtr]::Zero) {
            $null = [MenuSmokeWin32]::GetWindowThreadProcessId($candidate, [ref]$owner)
            if ($owner -eq $previewProcess.Id) { $window = $candidate; break }
        }
        if ($previewProcess.HasExited) { throw "Preview exited early: $($previewProcess.ExitCode)" }
        Start-Sleep -Milliseconds 100
    }
    Assert-Condition ($window -ne [IntPtr]::Zero) 'Preview window did not appear.'
    # STARTUPINFO's hidden flag overrides an app's first ShowWindow call. The
    # second call displays only the interactive preview selected by this test.
    $null = [MenuSmokeWin32]::ShowWindow($window, 5)
    Select-Page $window 101
    foreach ($id in @(200, 201, 203, 204, 240, 241, 242, 243)) {
        $control = [MenuSmokeWin32]::GetDlgItem($window, $id)
        Assert-Condition ($control -ne [IntPtr]::Zero) "Health control $id missing."
        Assert-Condition (-not [MenuSmokeWin32]::IsWindowEnabled($control)) "Preview incorrectly enables gameplay control $id."
        Assert-Condition ([MenuSmokeWin32]::IsWindowVisible($control)) "Health control $id is hidden on Health page."
    }
    Save-MenuImage $window 'preview-health.png'
    Select-Page $window 102
    foreach ($id in @(206, 211, 212, 213, 214, 215, 216, 217, 218, 219)) {
        Assert-Condition (-not [MenuSmokeWin32]::IsWindowEnabled([MenuSmokeWin32]::GetDlgItem($window, $id))) "Preview incorrectly enables position/hero control $id."
    }
    Select-Page $window 103
    Assert-Condition (-not [MenuSmokeWin32]::IsWindowEnabled([MenuSmokeWin32]::GetDlgItem($window, 220))) 'Preview incorrectly enables game-window modification.'
    Select-Page $window 104
    Assert-Condition ([MenuSmokeWin32]::IsWindowEnabled([MenuSmokeWin32]::GetDlgItem($window, 230))) 'Diagnostics export unavailable.'
    Select-Page $window 230
    $diagnostics = Get-Content -LiteralPath (Join-Path $project 'diagnostics.json') -Raw | ConvertFrom-Json
    Assert-Condition ($diagnostics.preview -eq $true) 'Diagnostics does not identify preview mode.'
    Assert-Condition ($diagnostics.supported_build -eq $false) 'Preview incorrectly reports a supported game binding.'
    Assert-Condition ($diagnostics.hero_available -eq $false) 'Preview incorrectly reports a game actor.'
    Select-Page $window 100
    Save-MenuImage $window 'preview-overview.png'
    $null = [MenuSmokeWin32]::SendMessage($window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
    Assert-Condition ($previewProcess.WaitForExit(5000)) 'Preview failed to close.'
    Assert-Condition ($previewProcess.ExitCode -eq 0) "Preview exited with code $($previewProcess.ExitCode)."
    [pscustomobject]@{
        PreviewExitCode = $previewProcess.ExitCode
        HealthControlsDisabledWithoutGame = $true
        HeroPositionControlsDisabledWithoutGame = $true
        WindowControlsDisabledWithoutGame = $true
        DiagnosticsExportPassed = $true
        PagesNavigated = 5
        Screenshot = 'preview-health.png'
        GameStarted = $false
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $verification 'preview-test.json') -Encoding UTF8
    Write-Host 'PASS: compiled preview navigation, unavailable-control guards, diagnostics export, screenshots, and clean exit.'
} finally {
    if (-not $previewProcess.HasExited) { Stop-Process -Id $previewProcess.Id }
    $previewProcess.Dispose()
}
