$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Split-Path -Parent $PSScriptRoot
$compiler = 'C:\msys64\mingw64\bin\g++.exe'
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { throw "Compiler missing: $compiler" }
$savedPath = $env:PATH
$started = [DateTime]::UtcNow.ToString('o')
$inputs = @('src\debug_settings.cpp', 'src\debug_settings.h', 'src\debug_settings_bindings.inc', 'src\supported_builds.h',
    'src\engine_dispatch.h', 'tools\generate_debug_settings.py', 'tools\debug_settings_fixture.cpp',
    'tools\test_debug_settings.ps1', 'tools\supported_builds.py')
$inputHashes = [ordered]@{}
foreach ($relative in $inputs) {
    $inputHashes[$relative] = (Get-FileHash -LiteralPath (Join-Path $project $relative) -Algorithm SHA256).Hash.ToLowerInvariant()
}
try {
    $env:PATH = (Split-Path -Parent $compiler) + ';' + $savedPath
    $target = Join-Path $project 'verification\debug-settings-fixture.exe'
    & $compiler -std=c++17 -O2 -Wall -Wextra -pedantic -static -static-libgcc -static-libstdc++ (Join-Path $PSScriptRoot 'debug_settings_fixture.cpp') -o $target -lbcrypt
    if ($LASTEXITCODE -ne 0) { throw 'Debug-settings fixture compilation failed.' }
    $rawReport = & $target
    $fixtureExit = $LASTEXITCODE
    foreach ($relative in $inputs) {
        if ((Get-FileHash -LiteralPath (Join-Path $project $relative) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $inputHashes[$relative]) {
            throw "Verification input changed during fixture run: $relative"
        }
    }
    $report = $rawReport | ConvertFrom-Json
    $report | Add-Member -NotePropertyName verification_started_utc -NotePropertyValue $started
    $report | Add-Member -NotePropertyName input_sha256 -NotePropertyValue $inputHashes
    $report | Add-Member -NotePropertyName fixture_sha256 -NotePropertyValue (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
    $serialized = $report | ConvertTo-Json -Depth 8
    $serialized | Set-Content -LiteralPath (Join-Path $project 'verification\debug-settings-fixture-results.json') -Encoding UTF8
    Write-Output $serialized
    if ($fixtureExit -ne 0) { throw 'Debug-settings fixture failed.' }
} finally { $env:PATH = $savedPath }
