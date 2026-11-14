[CmdletBinding()]
param([string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Split-Path -Parent $PSScriptRoot
$game = Join-Path (Split-Path -Parent $project) 'Spider-Man2.exe'
. (Join-Path $PSScriptRoot 'supported_builds.ps1')
$null = Get-SM2SupportedGameHash $game
$inputs = @('src\photo_mode_gate.cpp','src\photo_mode_gate.h','tools\photo_gate_fixture.cpp','tools\test_photo_gate.ps1') | ForEach-Object { Join-Path $project $_ }
$inputs += $game
$inputs += @((Join-Path $PSScriptRoot 'supported_builds.ps1'), (Join-Path (Join-Path $project 'src') 'supported_builds.h'))
$hashes = [ordered]@{}
foreach ($path in $inputs) { $hashes[$path] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
$executable = Join-Path $project 'verification\photo_gate_fixture.exe'
$arguments = @('-std=c++17','-O2','-Wall','-Wextra','-Werror','-DUNICODE','-D_UNICODE','-DNOMINMAX',
    '-static','-static-libgcc','-static-libstdc++','-o',$executable,(Join-Path $PSScriptRoot 'photo_gate_fixture.cpp'),'-luser32')
$savedPath = $env:Path
try {
    $env:Path = (Split-Path -Parent $Compiler) + ';' + $env:Path
    & $Compiler @arguments
    if ($LASTEXITCODE -ne 0) { throw "Photo gate fixture compilation failed: $LASTEXITCODE" }
    $raw = & $executable $game
    $testExit = $LASTEXITCODE
    if ($testExit -ne 0) { throw "Photo gate fixture failed: $testExit" }
    $result = ($raw -join "`n") | ConvertFrom-Json
} finally { $env:Path = $savedPath }
foreach ($path in $inputs) { if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hashes[$path]) { throw "Input changed: $path" } }
$report = [ordered]@{
    schema = 1; generated_utc = [DateTime]::UtcNow.ToString('o'); input_sha256 = $hashes
    compiler = $Compiler; compile_arguments = $arguments
    executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
    command = @($executable,$game); exit_code = $testExit
    scope = 'Actual 52-byte retail Open code copied into isolated CPU host, synthetic photo object/native CanEnter continuation, real production wrapper and lifecycle with protection fault injection. No game, GUI, input or engine calls.'
    result = $result
}
$report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $project 'verification\photo-gate-tests.json') -Encoding UTF8
$report | ConvertTo-Json -Depth 10
if (-not $result.passed) { throw 'Photo gate fixture did not pass.' }
