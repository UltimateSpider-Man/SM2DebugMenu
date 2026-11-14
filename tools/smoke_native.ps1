[CmdletBinding()]
param([string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Split-Path -Parent $PSScriptRoot
$hostExe = Join-Path $project 'dist\SM2DebugMenuSmokeHost.exe'
$savedCompilerPath = $env:Path
try {
    $env:Path = (Split-Path -Parent $Compiler) + ';' + $env:Path
    & $Compiler '-std=c++17' '-O2' '-Wall' '-Wextra' '-static' '-municode' '-mwindows' (Join-Path $PSScriptRoot 'smoke_host.cpp') '-o' $hostExe '-luser32'
    if ($LASTEXITCODE -ne 0) { throw 'Native smoke host compilation failed.' }
} finally { $env:Path = $savedCompilerPath }
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class NativeSmokeWin32 {
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls,string title);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd,int cmd);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hwnd,uint msg,IntPtr wp,IntPtr lp);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr hwnd,int id);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd,out uint pid);
    [DllImport("user32.dll",EntryPoint="GetWindowLongPtrW")] public static extern IntPtr GetWindowLong(IntPtr hwnd,int index);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd,out Rect rect);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
}
'@
function Assert-Native([bool]$Ok,[string]$Message) { if(-not $Ok){throw $Message} }
function Find-OwnWindow([string]$Class,[string]$Title,[int]$ProcessId) {
    $window = [NativeSmokeWin32]::FindWindow($Class,$Title)
    $owner = [uint32]0
    if($window -ne [IntPtr]::Zero) { $null=[NativeSmokeWin32]::GetWindowThreadProcessId($window,[ref]$owner) }
    if($owner -eq $ProcessId){return $window}
    return [IntPtr]::Zero
}
$nativeProcess=Start-Process -FilePath $hostExe -WindowStyle Hidden -PassThru
try {
    $deadline=[DateTime]::UtcNow.AddSeconds(10)
    $hostWindow=[IntPtr]::Zero; $menuWindow=[IntPtr]::Zero
    while([DateTime]::UtcNow -lt $deadline) {
        $hostWindow=Find-OwnWindow 'SM2DebugMenuSmokeHost' 'SM2 Debug Menu smoke host' $nativeProcess.Id
        if($hostWindow -ne [IntPtr]::Zero){$null=[NativeSmokeWin32]::ShowWindow($hostWindow,5)}
        $menuWindow=Find-OwnWindow 'SM2DebugMenuWindow' 'SM2 Debug Menu - F10' $nativeProcess.Id
        if($hostWindow -ne [IntPtr]::Zero -and $menuWindow -ne [IntPtr]::Zero){break}
        if($nativeProcess.HasExited){throw "Native host exited early: $($nativeProcess.ExitCode)"}
        Start-Sleep -Milliseconds 100
    }
    Assert-Native ($menuWindow -ne [IntPtr]::Zero) 'DLL C entry failed to create a menu.'
    $null=[NativeSmokeWin32]::ShowWindow($menuWindow,5)
    Start-Sleep -Milliseconds 500
    foreach($id in @(200,201,203,240,241,242,243,206,212,214)) {
        Assert-Native (-not [NativeSmokeWin32]::IsWindowEnabled([NativeSmokeWin32]::GetDlgItem($menuWindow,$id))) "Wrong-process guard failed for control $id."
    }
    $originalStyle=[NativeSmokeWin32]::GetWindowLong($hostWindow,-16)
    $originalRect=New-Object NativeSmokeWin32+Rect
    $null=[NativeSmokeWin32]::GetWindowRect($hostWindow,[ref]$originalRect)
    $null=[NativeSmokeWin32]::SendMessage($menuWindow,0x111,[IntPtr]103,[IntPtr]::Zero)
    Assert-Native ([NativeSmokeWin32]::IsWindowEnabled([NativeSmokeWin32]::GetDlgItem($menuWindow,220))) 'Window-thread dispatcher did not connect.'
    $null=[NativeSmokeWin32]::SendMessage($menuWindow,0x111,[IntPtr]220,[IntPtr]::Zero)
    Start-Sleep -Milliseconds 500
    $borderlessStyle=[NativeSmokeWin32]::GetWindowLong($hostWindow,-16)
    Assert-Native ($borderlessStyle -ne $originalStyle) 'Borderless action did not change the host window.'
    Assert-Native ([NativeSmokeWin32]::IsWindowEnabled([NativeSmokeWin32]::GetDlgItem($menuWindow,221))) 'Restore-window action was not enabled.'
    $null=[NativeSmokeWin32]::SendMessage($menuWindow,0x111,[IntPtr]221,[IntPtr]::Zero)
    Start-Sleep -Milliseconds 500
    Assert-Native ([NativeSmokeWin32]::GetWindowLong($hostWindow,-16) -eq $originalStyle) 'Original window style was not restored.'
    $restoredRect=New-Object NativeSmokeWin32+Rect
    $null=[NativeSmokeWin32]::GetWindowRect($hostWindow,[ref]$restoredRect)
    Assert-Native ($restoredRect.Left -eq $originalRect.Left -and $restoredRect.Top -eq $originalRect.Top -and $restoredRect.Right -eq $originalRect.Right -and $restoredRect.Bottom -eq $originalRect.Bottom) 'Original window rectangle was not restored.'
    $null=[NativeSmokeWin32]::SendMessage($menuWindow,0x111,[IntPtr]104,[IntPtr]::Zero)
    $null=[NativeSmokeWin32]::SendMessage($menuWindow,0x111,[IntPtr]230,[IntPtr]::Zero)
    Start-Sleep -Milliseconds 300
    $diagnostics=Get-Content -LiteralPath (Join-Path $project 'diagnostics.json') -Raw | ConvertFrom-Json
    Assert-Native ($diagnostics.preview -eq $false -and $diagnostics.supported_build -eq $false -and $diagnostics.hero_available -eq $false) 'DLL wrong-process diagnostics guard failed.'
    $null=[NativeSmokeWin32]::SendMessage($hostWindow,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
    Assert-Native ($nativeProcess.WaitForExit(5000)) 'Native host failed to close.'
    Assert-Native ($nativeProcess.ExitCode -eq 0) 'Native host returned an error.'
    [pscustomobject]@{
        DllCEntryPassed=$true; UnsupportedProcessGameplayDisabled=$true
        WindowThreadDispatchPassed=$true; BorderlessAndRestorePassed=$true
        DiagnosticsPassed=$true; HostExitCode=$nativeProcess.ExitCode; ActualGameStarted=$false
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $project 'verification\native-test.json') -Encoding UTF8
    Write-Host 'PASS: native DLL entry, wrong-process guards, window-thread dispatch, borderless/restore, export, and host exit.'
} finally {
    if(-not $nativeProcess.HasExited){Stop-Process -Id $nativeProcess.Id}
    $nativeProcess.Dispose()
}
