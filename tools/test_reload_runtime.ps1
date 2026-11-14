[CmdletBinding()]
param([string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Split-Path -Parent $PSScriptRoot
$game = Join-Path (Split-Path -Parent $project) 'Spider-Man2.exe'
. (Join-Path $PSScriptRoot 'supported_builds.ps1')
$expectedGame = Get-SM2SupportedGameHash $game
$output = Join-Path $project 'verification\reload-runtime-fixture'
$null = [IO.Directory]::CreateDirectory($output)
$fixture = Join-Path $PSScriptRoot 'reload_runtime_fixture.cpp'
$runtime = Join-Path $project 'src\reload_runtime.cpp'
$header = Join-Path $project 'src\reload_runtime.h'
$dispatcher = Join-Path $project 'src\engine_dispatch.h'
$inputs = @($fixture,$runtime,$header,$dispatcher)
$inputs += @((Join-Path $PSScriptRoot 'supported_builds.ps1'), (Join-Path (Join-Path $project 'src') 'supported_builds.h'))
$hashes = [ordered]@{}
foreach ($path in $inputs) { $hashes[$path] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
$executable = Join-Path $output 'reload_runtime_fixture.exe'
$arguments = @('-std=c++17','-O2','-Wall','-Wextra','-Werror','-DUNICODE','-D_UNICODE','-DNOMINMAX',
    '-municode','-static','-static-libgcc','-static-libstdc++','-Wl,--no-insert-timestamp',$fixture,'-o',$executable,'-luser32')
$oldCompilerPath = $env:Path
try {
    $env:Path = (Split-Path -Parent $Compiler) + ';' + $env:Path
    & $Compiler @arguments
    if ($LASTEXITCODE -ne 0) { throw "Reload runtime fixture compilation failed: $LASTEXITCODE" }
    $raw = & $executable $game
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) { throw "Reload runtime fixture failed: $exitCode" }
    $result = ($raw -join "`n") | ConvertFrom-Json
} finally { $env:Path = $oldCompilerPath }
foreach ($path in $inputs) {
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hashes[$path]) {
        throw "Fixture input changed: $path"
    }
}
$report = [ordered]@{
    schema = 1
    generated_utc = [DateTime]::UtcNow.ToString('o')
    runtime_sha256 = $hashes[$runtime]
    header_sha256 = $hashes[$header]
    input_sha256 = $hashes
    game_file_sha256 = $expectedGame
    executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
    compiler = $Compiler
    compile_arguments = $arguments
    command = @($executable,$game)
    exit_code = $exitCode
    game_launched = $false
    real_engine_context_proven = $false
    result = $result
}
$reportPath = Join-Path $output 'reload-runtime-tests.json'
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $reportPath -Encoding UTF8
$report | ConvertTo-Json -Depth 12
if (-not $result.passed) { throw "Reload runtime fixture failed; inspect $reportPath" }
