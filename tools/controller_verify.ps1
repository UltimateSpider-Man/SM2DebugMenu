[CmdletBinding()]
param(
    [string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe',
    [ValidateRange(100, 10000)][int]$ProbeMilliseconds = 3100,
    [switch]$SkipHardwareProbe,
    [switch]$Preview
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$source = Join-Path (Split-Path -Parent $PSScriptRoot) 'src\controller_input.cpp'
$header = Join-Path (Split-Path -Parent $PSScriptRoot) 'src\controller_input.h'
$tests = Join-Path $PSScriptRoot 'controller_tests.cpp'
$executable = Join-Path $PSScriptRoot $(if ($Preview) { 'controller_preview_tests.exe' } else { 'controller_tests.exe' })
foreach ($file in @($Compiler, $source, $header, $tests)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Required file missing: $file" }
}

$savedCompilerPath = $env:Path
try {
    $env:Path = (Split-Path -Parent $Compiler) + ';' + $env:Path
    $arguments = @('-std=c++17', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-DUNICODE', '-D_UNICODE',
        '-DNOMINMAX', '-static', '-static-libgcc', '-static-libstdc++')
    if ($Preview) { $arguments += '-DSM2DEBUG_PREVIEW' }
    $arguments += @($source, $tests, '-o', $executable, '-lhid', '-lsetupapi', '-luser32')
    & $Compiler @arguments
    if ($LASTEXITCODE -ne 0) { throw "Controller test build failed: $LASTEXITCODE" }
    $testOutput = & $executable
    if ($LASTEXITCODE -ne 0) { throw "Controller tests failed: $LASTEXITCODE" }
    $testResult = ($testOutput -join [Environment]::NewLine) | ConvertFrom-Json
    if (-not $testResult.passed) { throw 'Controller tests did not report success.' }

    $probeResult = $null
    if (-not $SkipHardwareProbe) {
        # No HWND is passed by this tool, so it cannot enqueue game menu input.
        # It opens input-only Sony HID handles, reads reports, and cancels reads.
        $probeOutput = & $executable --probe $ProbeMilliseconds
        if ($LASTEXITCODE -ne 0) { throw "Read-only controller probe failed: $LASTEXITCODE" }
        $probeResult = ($probeOutput -join [Environment]::NewLine) | ConvertFrom-Json
        if ($probeResult.queued_menu_actions -ne 0 -or $probeResult.after_shutdown_active -ne 0) {
            throw 'The controller probe violated its no-input/clean-shutdown contract.'
        }
    }
    $result = [ordered]@{
        schema = 1
        preview_build = [bool]$Preview
        generated_at_utc = [DateTime]::UtcNow.ToString('o')
        source_sha256 = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
        header_sha256 = (Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash.ToLowerInvariant()
        test_sha256 = (Get-FileHash -LiteralPath $tests -Algorithm SHA256).Hash.ToLowerInvariant()
        deterministic = $testResult
        read_only_hardware = $probeResult
        physical_ps_button_tested = $false
        game_launched = $false
    }
    $json = $result | ConvertTo-Json -Depth 8
    $resultName = if ($Preview) { 'controller_preview_results.json' } else { 'controller_results.json' }
    [IO.File]::WriteAllText((Join-Path $PSScriptRoot $resultName), $json,
        (New-Object Text.UTF8Encoding($false)))
    $json
} finally {
    $env:Path = $savedCompilerPath
}
