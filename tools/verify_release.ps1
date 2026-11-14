[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell to verify this x64 release.' }
$project = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $project 'dist\sm2debugmenu.dll'
$package = Join-Path $project 'dist\SM2DebugMenu.script'
$preview = Join-Path $project 'dist\SM2DebugMenuPreview.exe'
$metadata = Get-Content -LiteralPath (Join-Path $project 'info.json') -Raw | ConvertFrom-Json
$dllHash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant()
$bytes = [IO.File]::ReadAllBytes($dll)
$pe = [BitConverter]::ToInt32($bytes, 60)
if ($bytes[0] -ne 77 -or $bytes[1] -ne 90 -or $pe -lt 64 -or $pe + 6 -gt $bytes.Length -or
    [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664) { throw 'Release DLL is not an x64 PE image.' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($package)
try {
    if ($archive.Entries.Count -ne 2) { throw 'Unexpected package contents.' }
    $payload = $archive.GetEntry('sm2debugmenu.dll')
    $info = $archive.GetEntry('info.json')
    if ($null -eq $payload -or $null -eq $info) { throw 'Package payload or metadata is missing.' }
    $stream = $payload.Open()
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $payloadHash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
    if ($payloadHash -cne $dllHash) { throw 'Packaged DLL differs from build output.' }
    $reader = New-Object IO.StreamReader($info.Open())
    try { $packagedInfo = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
    if ($packagedInfo.version -cne $metadata.version -or $packagedInfo.name -cne 'SM2DebugMenu') {
        throw 'Packaged version or name differs from source metadata.'
    }
} finally { $archive.Dispose() }
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SM2ReleaseLoaderCheck {
 [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr LoadLibraryW(string path);
 [DllImport("kernel32.dll", CharSet=CharSet.Ansi, ExactSpelling=true, SetLastError=true)] public static extern IntPtr GetProcAddress(IntPtr module, string name);
 [DllImport("kernel32.dll", SetLastError=true)] public static extern bool FreeLibrary(IntPtr module);
}
'@
$module = [SM2ReleaseLoaderCheck]::LoadLibraryW($dll)
if ($module -eq [IntPtr]::Zero) { throw "Loader rejected release DLL (Win32 $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))." }
$exportPresent = $false
$released = $false
try {
    # Resolve only. Calling script_enable would start overlay/game workers.
    $exportPresent = [SM2ReleaseLoaderCheck]::GetProcAddress($module, 'script_enable') -ne [IntPtr]::Zero
    if (-not $exportPresent) { throw 'The script_enable loader export is missing.' }
} finally { $released = [SM2ReleaseLoaderCheck]::FreeLibrary($module) }
if (-not $released) { throw 'The validation host could not release the module.' }
$report = [ordered]@{
    schema = 1
    generated_utc = [DateTime]::UtcNow.ToString('o')
    version = $metadata.version
    passed = $true
    dll_sha256 = $dllHash
    package_sha256 = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant()
    preview_sha256 = (Get-FileHash -LiteralPath $preview -Algorithm SHA256).Hash.ToLowerInvariant()
    package_dll_matches = $true
    imports_resolved_on_this_os = $true
    os_version = [Environment]::OSVersion.Version.ToString()
    export_present = $exportPresent
    export_invoked = $false
    game_started = $false
    module_released = $released
}
$reportPath = Join-Path $project 'verification\release-package-tests.json'
$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $reportPath -Encoding UTF8
$report | ConvertTo-Json -Depth 5
