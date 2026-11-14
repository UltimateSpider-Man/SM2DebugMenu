[CmdletBinding()]
param([string]$GameDirectory)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([String]::IsNullOrWhiteSpace($GameDirectory)) {
    $GameDirectory = Split-Path -Parent $PSScriptRoot
}

$running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -in @('Spider-Man2', "Marvel's Spider-Man 2") })
if ($running.Count -gt 0) { throw "Close Spider-Man 2 before installing the menu (running PID: $($running.Id -join ', '))." }
$game = (Resolve-Path -LiteralPath $GameDirectory).ProviderPath
if (-not (Test-Path -LiteralPath (Join-Path $game 'Spider-Man2.exe') -PathType Leaf)) {
    throw "Spider-Man2.exe was not found in '$game'. Pass -GameDirectory with this game's folder."
}
if (-not (Test-Path -LiteralPath (Join-Path $game 'winmm.dll') -PathType Leaf)) {
    throw 'The existing winmm.dll script loader is missing. Install Overstrike script support before installing this menu.'
}
$sourceDll = Join-Path $PSScriptRoot 'dist\sm2debugmenu.dll'
$sourcePackage = Join-Path $PSScriptRoot 'dist\SM2DebugMenu.script'
foreach ($path in @($sourceDll, $sourcePackage)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Build output missing: $path. Run build.ps1 first." }
}
$bytes = [IO.File]::ReadAllBytes($sourceDll)
if ($bytes.Length -lt 256 -or $bytes[0] -ne 77 -or $bytes[1] -ne 90) { throw 'The menu DLL is not a valid Windows binary. Rebuild it.' }
$pe = [BitConverter]::ToInt32($bytes, 60)
if ($pe -lt 0 -or $pe + 6 -gt $bytes.Length -or [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664) {
    throw 'The menu DLL must be built for x64 Windows. Rebuild it with the MinGW-w64 x64 compiler.'
}

$statePath = Join-Path $PSScriptRoot 'install-state.json'
$oldState = $null
if (Test-Path -LiteralPath $statePath -PathType Leaf) {
    $oldState = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    if (-not [String]::Equals($oldState.GameDirectory, $game, [StringComparison]::OrdinalIgnoreCase)) {
        throw "An installation is already recorded in '$($oldState.GameDirectory)'. Uninstall it before choosing a different folder."
    }
}

$scriptList = Join-Path $game 'scripts.txt'
$targetDll = Join-Path $game 'scripts\sm2debugmenu.dll'
$rootDll = Join-Path $game 'sm2debugmenu.dll'
$sourceDllHash = (Get-FileHash -LiteralPath $sourceDll -Algorithm SHA256).Hash
$quarantineRootDll = Test-Path -LiteralPath $rootDll
if (Test-Path -LiteralPath $rootDll) {
    if (-not (Test-Path -LiteralPath $rootDll -PathType Leaf) -or
        ((Get-Item -LiteralPath $rootDll -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'The root menu DLL is not a regular file. It was preserved; no files were changed.'
    }
    # A bare LoadLibrary name can resolve the application directory before scripts.
    # The legacy hash is the verified previous 2.8.1 release retained in backups.
    $recognizedRootHashes = @($sourceDllHash, 'f69cace5f111d3a2f5370b3a8aa68e2d5c848b87b95cc810e25ec1bece8e077b')
    if ($null -ne $oldState) { $recognizedRootHashes += $oldState.DllSha256 }
    $rootDllHash = (Get-FileHash -LiteralPath $rootDll -Algorithm SHA256).Hash
    if ($recognizedRootHashes -notcontains $rootDllHash) {
        throw 'An unrecognized root sm2debugmenu.dll would shadow the installed menu. It was preserved; no files were changed.'
    }
}
$targetPackage = Join-Path $game 'Mods Library\SM2DebugMenu.script'
$launcher = Join-Path $game 'Launch SM2 Debug Menu.cmd'
$launcherText = "@echo off`r`nsetlocal DisableDelayedExpansion`r`ncd /d `"%~dp0`"`r`nif errorlevel 1 exit /b 1`r`nif exist `"%~dp0LTSC2019Compatibility\install-state.json`" (`r`n  if not exist `"%~dp0Launch SM2 LTSC 2019.cmd`" (`r`n    echo The installed LTSC launcher is missing. The game was not started.`r`n    exit /b 1`r`n  )`r`n  `"%~dp0Launch SM2 LTSC 2019.cmd`" %*`r`n) else (`r`n  start `"`" `"%~dp0Spider-Man2.exe`" -scripts %*`r`n)`r`nendlocal`r`n"
$latin1 = [Text.Encoding]::GetEncoding(28591)
$originalText = if (Test-Path -LiteralPath $scriptList -PathType Leaf) { $latin1.GetString([IO.File]::ReadAllBytes($scriptList)) } else { '' }
$newlineMatch = [regex]::Match($originalText, '\r\n|\n|\r')
$newline = if ($newlineMatch.Success) { $newlineMatch.Value } else { "`r`n" }
$newText = ''
foreach ($match in [regex]::Matches($originalText, '[^\r\n]*(?:\r\n|\n|\r|$)')) {
    if ($match.Length -eq 0) { continue }
    $line = [regex]::Replace($match.Value, '\r\n$|\n$|\r$', '')
    if (-not [String]::Equals($line, 'sm2debugmenu.dll', [StringComparison]::OrdinalIgnoreCase)) { $newText += $match.Value }
}
if ($newText.Length -gt 0 -and -not ($newText.EndsWith("`n") -or $newText.EndsWith("`r"))) { $newText += $newline }
$newText += 'sm2debugmenu.dll' + $newline

$backup = Join-Path (Join-Path $PSScriptRoot 'backups') ([DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 6))
$null = New-Item -ItemType Directory -Path $backup -Force
$files = @(
    @{ Key = 'Scripts'; Path = $scriptList; Name = 'scripts.txt' },
    @{ Key = 'Dll'; Path = $targetDll; Name = 'sm2debugmenu.dll' },
    @{ Key = 'Package'; Path = $targetPackage; Name = 'SM2DebugMenu.script' },
    @{ Key = 'Launcher'; Path = $launcher; Name = 'Launch SM2 Debug Menu.cmd' },
    @{ Key = 'State'; Path = $statePath; Name = 'install-state.json' }
)
if ($quarantineRootDll) { $files += @{ Key = 'RootDll'; Path = $rootDll; Name = 'root-sm2debugmenu.dll' } }
$saved = @()
foreach ($file in $files) {
    if ((Test-Path -LiteralPath $file.Path) -and -not (Test-Path -LiteralPath $file.Path -PathType Leaf)) { throw "Expected a file, but found a directory: $($file.Path)" }
    $existed = Test-Path -LiteralPath $file.Path -PathType Leaf
    $copy = Join-Path $backup $file.Name
    if ($existed) { Copy-Item -LiteralPath $file.Path -Destination $copy }
    $saved += [pscustomobject]@{ Key = $file.Key; Path = $file.Path; Existed = $existed; Backup = $copy }
}
$manifest = [pscustomobject]@{ GameDirectory = $game; CreatedUtc = [DateTime]::UtcNow.ToString('o'); Files = $saved }
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $backup 'manifest.json') -Encoding UTF8

try {
    $null = New-Item -ItemType Directory -Path (Split-Path -Parent $targetDll), (Split-Path -Parent $targetPackage) -Force
    Copy-Item -LiteralPath $sourceDll -Destination $targetDll -Force
    if ($quarantineRootDll) {
        $rootSaved = @($saved | Where-Object { $_.Key -eq 'RootDll' })[0]
        if ((Get-FileHash -LiteralPath $rootDll -Algorithm SHA256).Hash -cne $rootDllHash -or
            (Get-FileHash -LiteralPath $rootSaved.Backup -Algorithm SHA256).Hash -cne $rootDllHash) {
            throw 'The root menu DLL changed after its backup. It was not quarantined.'
        }
        Remove-Item -LiteralPath $rootDll -Force
    }
    Copy-Item -LiteralPath $sourcePackage -Destination $targetPackage -Force
    [IO.File]::WriteAllBytes($scriptList, $latin1.GetBytes($newText))
    [IO.File]::WriteAllBytes($launcher, [Text.Encoding]::ASCII.GetBytes($launcherText))
    $originals = if ($null -ne $oldState) { @($oldState.Originals) } else { @($saved | Where-Object { $_.Key -notin @('State','RootDll') }) }
    $state = [pscustomobject]@{
        GameDirectory = $game
        InstalledUtc = [DateTime]::UtcNow.ToString('o')
        Originals = $originals
        DllSha256 = (Get-FileHash -LiteralPath $targetDll -Algorithm SHA256).Hash
        PackageSha256 = (Get-FileHash -LiteralPath $targetPackage -Algorithm SHA256).Hash
        LauncherSha256 = (Get-FileHash -LiteralPath $launcher -Algorithm SHA256).Hash
    }
    $quarantines = @()
    if ($null -ne $oldState -and $oldState.PSObject.Properties['RootDllQuarantines']) { $quarantines = @($oldState.RootDllQuarantines) }
    if ($quarantineRootDll) {
        $quarantines += [pscustomobject]@{ Path=$rootDll; Backup=$rootSaved.Backup;
            Sha256=(Get-FileHash -LiteralPath $rootSaved.Backup -Algorithm SHA256).Hash }
    }
    if ($quarantines.Count) { $state | Add-Member -NotePropertyName RootDllQuarantines -NotePropertyValue $quarantines }
    $state | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $statePath -Encoding UTF8
} catch {
    $failure = $_
    $rollbackErrors = @()
    foreach ($file in $saved) {
        try {
            if ($file.Existed) { Copy-Item -LiteralPath $file.Backup -Destination $file.Path -Force }
            elseif (Test-Path -LiteralPath $file.Path -PathType Leaf) { Remove-Item -LiteralPath $file.Path -Force }
        } catch { $rollbackErrors += $_.Exception.Message }
    }
    if ($rollbackErrors.Count -gt 0) { throw "Installation failed: $failure. Rollback needs attention: $($rollbackErrors -join '; '). Backups: $backup" }
    throw "Installation failed and previous files were restored: $failure"
}

Write-Host "Installed the debug menu in '$game'."
Write-Host "Launch with: $launcher"
Write-Host "Backups: $backup"
Write-Host 'Overstrike reinstalls may replace scripts.txt; enable the SM2DebugMenu.script package in the correct game profile when using Overstrike.'
