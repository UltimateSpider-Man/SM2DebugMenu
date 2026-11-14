[CmdletBinding()]
param([string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Split-Path -Parent $PSScriptRoot
$output = Join-Path $project 'verification\hero-actions-fixture'
$null = [IO.Directory]::CreateDirectory($output)
$fixture = Join-Path $PSScriptRoot 'backend_performance_fixture.cpp'
$heroFixture = Join-Path $PSScriptRoot 'hero_actions_fixture.cpp'
$rawFixture = Join-Path $PSScriptRoot 'raw_snapshot_fixture.cpp'
$backend = Join-Path $project 'src\backend.cpp'
$header = Join-Path $project 'src\debugmenu.h'
$dispatcherHeader = Join-Path $project 'src\engine_dispatch.h'
$settings = Join-Path $project 'src\debug_settings.cpp'
$settingsHeader = Join-Path $project 'src\debug_settings.h'
$settingsBindings = Join-Path $project 'src\debug_settings_bindings.inc'
$reloadHeader = Join-Path $project 'src\reload_runtime.h'
$primaryHeader = Join-Path $project 'src\primary_objective_runtime.h'
$actorSaveHeader = Join-Path $project 'src\actor_save_runtime.h'
$replayHeader = Join-Path $project 'src\replay_runtime.h'
$catalog = Join-Path $project 'src\reload_mission_catalog.cpp'
$catalogHeader = Join-Path $project 'src\reload_mission_catalog.h'
$capture = Join-Path $project 'src\game_input_capture.cpp'
$captureHeader = Join-Path $project 'src\game_input_capture.h'
$photoGate = Join-Path $project 'src\photo_mode_gate.cpp'
$photoGateHeader = Join-Path $project 'src\photo_mode_gate.h'
$overlayHeader = Join-Path $project 'src\overlay.h'
$inputs = @($fixture, $heroFixture, $rawFixture, $backend, $header, $dispatcherHeader, $settings, $settingsHeader, $settingsBindings,
    $reloadHeader, $primaryHeader, $actorSaveHeader, $replayHeader, $catalog, $catalogHeader, $capture, $captureHeader, $photoGate, $photoGateHeader, $overlayHeader)
$hashes = [ordered]@{}
$inputs += (Join-Path (Join-Path $project 'src') 'supported_builds.h')
foreach ($path in @($Compiler) + $inputs) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required fixture input missing: $path" }
}
foreach ($path in $inputs) { $hashes[$path] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
$executable = Join-Path $output 'hero_actions_fixture.exe'
$compileArguments = @('-std=c++17','-O2','-Wall','-Wextra','-Werror','-DUNICODE','-D_UNICODE','-DNOMINMAX','-DEXISTING_HERO_CASES',
    '-static','-static-libgcc','-static-libstdc++','-Wl,--no-insert-timestamp','-o',$executable,$fixture,$settings,$catalog,$capture,$photoGate,
    '-luser32','-lpsapi','-lbcrypt','-lhid')
$oldCompilerPath = $env:Path
try {
    $env:Path = (Split-Path -Parent $Compiler) + ';' + $env:Path
    & $Compiler @compileArguments
    if ($LASTEXITCODE -ne 0) { throw "Hero action fixture compilation failed: $LASTEXITCODE" }
    $raw = & $executable $output
    $code = $LASTEXITCODE
    $result = ($raw -join "`n") | ConvertFrom-Json
} finally { $env:Path = $oldCompilerPath }
foreach ($path in $inputs) {
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hashes[$path]) {
        throw "Fixture input changed during verification: $path. Rerun against frozen sources."
    }
}
$report = [ordered]@{
    schema = 1
    generated_utc = [DateTime]::UtcNow.ToString('o')
    backend_sha256 = $hashes[$backend]
    header_sha256 = $hashes[$header]
    fixture_sha256 = $hashes[$fixture]
    input_sha256 = $hashes
    executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
    compiler = $Compiler
    compile_arguments = $compileArguments
    command = @($executable, $output)
    exit_code = $code
    game_launched = $false
    real_engine_context_proven = $false
    reload_runtime_boundary = 'simulated guard/acceptance; production queue and actor lifetime logic'
    primary_runtime_boundary = 'simulated guard/acceptance; production typed queue and post-call lifetime logic'
    actor_save_runtime_boundary = 'simulated preflight and reset outcomes; production persistent-side-effect ordering'
    replay_runtime_boundary = 'simulated invoked/accepted outcomes; production typed queue and lifetime invalidation'
    mission_catalog = 'actual immutable production catalog linked, no game asset loading'
    os_clipboard_changed = $false
    result = $result
}
$reportPath = Join-Path $output 'hero-actions-tests.json'
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $reportPath -Encoding UTF8
$report | ConvertTo-Json -Depth 12
if ($code -ne 0 -or -not $result.passed) { throw "Hero action verification failed; inspect $reportPath" }
