[CmdletBinding()]
param([string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Split-Path -Parent $PSScriptRoot
$game = Split-Path -Parent $project
$retailExe = Join-Path $game 'Spider-Man2.exe'
. (Join-Path $PSScriptRoot 'supported_builds.ps1')
$expectedExe = Get-SM2SupportedGameHash $retailExe
$build = Join-Path $project 'verification\actor-save-runtime-fixture'
$null = [IO.Directory]::CreateDirectory($build)
$executable = Join-Path $build 'actor_save_runtime_fixture.exe'
$testSource = Join-Path $PSScriptRoot 'actor_save_runtime_fixture.cpp'
$sourceNames = @('actor_save_runtime.h','actor_save_runtime.cpp','reload_runtime.h','reload_runtime.cpp','engine_dispatch.h')
$inputs = @($testSource, (Join-Path $PSScriptRoot 'reload_runtime_fixture.cpp')) + @($sourceNames | ForEach-Object { Join-Path (Join-Path $project 'src') $_ })
$inputs += @((Join-Path $PSScriptRoot 'supported_builds.ps1'), (Join-Path (Join-Path $project 'src') 'supported_builds.h'))
$before = @{}
foreach ($path in @($Compiler) + $inputs) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required fixture input missing: $path" }
}
foreach ($path in $inputs) { $before[$path] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
$arguments = @('-std=c++17','-O2','-Wall','-Wextra','-Werror','-municode','-static','-static-libgcc','-static-libstdc++',
    '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',$testSource,'-o',$executable)
$oldPath = $env:Path
try {
    $env:Path = (Split-Path -Parent $Compiler) + ';' + $env:Path
    & $Compiler @arguments
    if ($LASTEXITCODE -ne 0) { throw "Actor-save fixture compile failed: $LASTEXITCODE" }
    $raw = & $executable $retailExe
    $testExit = $LASTEXITCODE
    $result = ($raw -join "`n") | ConvertFrom-Json
    foreach ($path in $before.Keys) {
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $before[$path]) { throw "Fixture input changed during verification: $path" }
    }
    $report = [ordered]@{
        schema = 1
        generated_at_utc = [DateTime]::UtcNow.ToString('o')
        result = $result
        exit_code = $testExit
        input_sha256 = $before
        executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
        retail_exe_sha256 = $expectedExe
        compiler = $Compiler
        compile_arguments = $arguments
        game_launched = $false
        real_native_calls = $false
        scope = 'Synthetic host; native reset and engine-callback substitutes. Real PE supplies code/RTTI pins only.'
        ordering = 'Before manual Reload; reset remains applied if a later native Reload refuses. No rollback is simulated.'
    }
    $json = $report | ConvertTo-Json -Depth 12
    [IO.File]::WriteAllText((Join-Path $project 'verification\actor-save-runtime-tests.json'), $json, (New-Object Text.UTF8Encoding $false))
    Write-Output $json
    if ($testExit -ne 0 -or -not $result.passed) { throw 'Actor-save fixture failed; see verification/actor-save-runtime-tests.json' }
} finally { $env:Path = $oldPath }
