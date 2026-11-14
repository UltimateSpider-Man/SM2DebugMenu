[CmdletBinding()]
param([string]$GameDirectory)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([String]::IsNullOrWhiteSpace($GameDirectory)) {
    $GameDirectory = Split-Path -Parent $PSScriptRoot
}

$running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -in @('Spider-Man2', "Marvel's Spider-Man 2") })
if ($running.Count -gt 0) { throw "Close Spider-Man 2 before uninstalling the menu (running PID: $($running.Id -join ', '))." }
$game = (Resolve-Path -LiteralPath $GameDirectory).ProviderPath
$statePath = Join-Path $PSScriptRoot 'install-state.json'
if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) {
    Write-Host 'No installation is recorded. Nothing was changed.'
    return
}
$state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
if (-not [String]::Equals($state.GameDirectory, $game, [StringComparison]::OrdinalIgnoreCase)) {
    throw "The installation belongs to '$($state.GameDirectory)'. Pass that folder as -GameDirectory."
}

$expectedPaths = @{
    Scripts = (Join-Path $game 'scripts.txt')
    Dll = (Join-Path $game 'scripts\sm2debugmenu.dll')
    Package = (Join-Path $game 'Mods Library\SM2DebugMenu.script')
    Launcher = (Join-Path $game 'Launch SM2 Debug Menu.cmd')
}
if (@($state.Originals).Count -ne 4 -or @($state.Originals | Select-Object -ExpandProperty Key -Unique).Count -ne 4) {
    throw 'The installation manifest is incomplete. No files were changed.'
}
foreach ($file in $state.Originals) {
    if (-not $expectedPaths.ContainsKey($file.Key) -or -not [String]::Equals($file.Path, $expectedPaths[$file.Key], [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The installation manifest has an unexpected target path. No files were changed.'
    }
    if ($file.Existed -and -not (Test-Path -LiteralPath $file.Backup -PathType Leaf)) { throw "Original backup missing: $($file.Backup). No files were changed." }
    if ((Test-Path -LiteralPath $file.Path) -and -not (Test-Path -LiteralPath $file.Path -PathType Leaf)) {
        throw "Expected a file, but found a directory: $($file.Path). No files were changed."
    }
}
$hashes = @{ Dll = $state.DllSha256; Package = $state.PackageSha256; Launcher = $state.LauncherSha256 }
$preserveLauncher = $false
foreach ($key in @('Dll', 'Package', 'Launcher')) {
    $path = $expectedPaths[$key]
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $hashes[$key]) {
            if ($key -eq 'Launcher') {
                $preserveLauncher = $true
                Write-Warning "The launcher changed after installation and will be preserved: $path"
                continue
            }
            throw "'$path' changed after installation. Move that file aside before uninstalling so your changes are preserved. No files were changed."
        }
    }
}

$latin1 = [Text.Encoding]::GetEncoding(28591)
$scriptList = $expectedPaths.Scripts
if (Test-Path -LiteralPath $scriptList -PathType Leaf) {
    $text = $latin1.GetString([IO.File]::ReadAllBytes($scriptList))
    $newText = ''
    foreach ($match in [regex]::Matches($text, '[^\r\n]*(?:\r\n|\n|\r|$)')) {
        if ($match.Length -eq 0) { continue }
        $line = [regex]::Replace($match.Value, '\r\n$|\n$|\r$', '')
        if (-not [String]::Equals($line, 'sm2debugmenu.dll', [StringComparison]::OrdinalIgnoreCase)) { $newText += $match.Value }
    }
    $original = @($state.Originals | Where-Object { $_.Key -eq 'Scripts' })[0]
    if ($original.Existed) {
        $oldText = $latin1.GetString([IO.File]::ReadAllBytes($original.Backup))
        if (-not ($oldText.EndsWith("`n") -or $oldText.EndsWith("`r"))) {
            foreach ($ending in @("`r`n", "`n", "`r")) {
                if ($newText -ceq ($oldText + $ending)) { $newText = $oldText; break }
            }
        }
        [IO.File]::WriteAllBytes($scriptList, $latin1.GetBytes($newText))
    } elseif ($newText.Length -eq 0) {
        Remove-Item -LiteralPath $scriptList -Force
    } else {
        [IO.File]::WriteAllBytes($scriptList, $latin1.GetBytes($newText))
    }
}

foreach ($key in @('Dll', 'Package', 'Launcher')) {
    if ($key -eq 'Launcher' -and $preserveLauncher) { continue }
    $path = $expectedPaths[$key]
    $original = @($state.Originals | Where-Object { $_.Key -eq $key })[0]
    if ($original.Existed) {
        $null = New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force
        Copy-Item -LiteralPath $original.Backup -Destination $path -Force
    }
    elseif (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
}
Remove-Item -LiteralPath $statePath -Force
Write-Host 'Debug menu uninstalled. Other scripts and backups were preserved.'
