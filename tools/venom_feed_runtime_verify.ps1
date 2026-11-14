[CmdletBinding()]
param([string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Split-Path -Parent $PSScriptRoot
$game = Split-Path -Parent $project
$retailExe = Join-Path $game 'Spider-Man2.exe'
. (Join-Path $PSScriptRoot 'supported_builds.ps1')
$expectedExe = Get-SM2SupportedGameHash $retailExe
$folder = Join-Path $project 'verification\venom-feed-runtime-fixture'
$null = [IO.Directory]::CreateDirectory($folder)
$executable = Join-Path $folder 'venom_feed_runtime_fixture.exe'
$testSource = Join-Path $PSScriptRoot 'venom_feed_runtime_fixture.cpp'
$inputs = @($PSCommandPath, $testSource) + @('venom_feed_runtime.h','venom_feed_runtime.cpp','reload_runtime.h','engine_dispatch.h','photo_mode_gate.h' | ForEach-Object { Join-Path (Join-Path $project 'src') $_ })
$inputs += @((Join-Path $PSScriptRoot 'supported_builds.ps1'), (Join-Path (Join-Path $project 'src') 'supported_builds.h'))
$hashes = @{}
foreach ($path in $inputs) { $hashes[$path] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
$arguments = @('-std=c++17','-O2','-Wall','-Wextra','-Werror','-municode','-static',$testSource,'-o',$executable)
$originalPath = $env:Path
try {
    $env:Path = (Split-Path -Parent $Compiler) + ';' + $env:Path
    & $Compiler @arguments
    if ($LASTEXITCODE -ne 0) { throw "Readiness fixture compile failed with code $LASTEXITCODE" }
    $output = & $executable $retailExe
    $code = $LASTEXITCODE
    if ($code -ne 0) { throw "Readiness fixture failed with code $code" }
    $result = ($output -join "`n") | ConvertFrom-Json
    if (-not $result.passed) { throw 'Readiness fixture did not pass.' }
    foreach ($path in $hashes.Keys) {
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hashes[$path]) { throw "Fixture input changed during verification: $path" }
    }
    $report = [ordered]@{
        schema = 1
        generated_at_utc = [DateTime]::UtcNow.ToString('o')
        result = $result
        exit_code = $code
        input_sha256 = $hashes
        retail_exe_sha256 = $expectedExe
        executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
        compiler_sha256 = (Get-FileHash -LiteralPath $Compiler -Algorithm SHA256).Hash.ToLowerInvariant()
        compile_arguments = $arguments
        scope = 'Synthetic host using production read-only module, real original PE code/RTTI pins and fixture-owned native-layout buffers. Callback/world/photo/window APIs are explicit substitutes. No game process is opened, no engine function is invoked.'
        installed_or_launched_game = $false
        native_actions = $false
        admission = 'Only a native finisher input request may be considered after fresh callback revalidation. A ready snapshot does not assert execution, kill or civilian receiver support.'
    }
    $json = $report | ConvertTo-Json -Depth 12
    [IO.File]::WriteAllText((Join-Path $project 'verification\venom-feed-runtime-tests.json'), $json, (New-Object Text.UTF8Encoding $false))
    Write-Output $json
} finally { $env:Path = $originalPath }
