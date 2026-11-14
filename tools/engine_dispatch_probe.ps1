[CmdletBinding()]
param([string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe', [switch]$SkipNegativeTests,
      [string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'supported_builds.ps1')
$game = Join-Path (Split-Path -Parent $project) 'Spider-Man2.exe'
$output = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $project 'verification\engine-dispatch-probe' }
$null = [IO.Directory]::CreateDirectory($output)
$dll = Join-Path $output 'engine_dispatch_probe.dll'
$hostExecutable = Join-Path $output 'engine_dispatch_probe_host.exe'
$source = Join-Path $project 'src\engine_dispatch.cpp'
$header = Join-Path $project 'src\engine_dispatch.h'
$entry = Join-Path $PSScriptRoot 'engine_dispatch_probe_entry.cpp'
$hostSource = Join-Path $PSScriptRoot 'engine_dispatch_probe_host.cpp'
$inspectorSource = Join-Path $PSScriptRoot 'engine_dispatch_probe_inspect.cpp'
$inspector = Join-Path $output 'engine_dispatch_probe_inspect.exe'
$inputs = @($source, $header, $entry, $hostSource, $inspectorSource)
$inputs += @((Join-Path $PSScriptRoot 'supported_builds.ps1'), (Join-Path (Join-Path $project 'src') 'supported_builds.h'))
& (Join-Path $PSScriptRoot 'engine_dispatch_probe_static.ps1') -GameExe $game -OutputPath (Join-Path $output 'static-binding-proof.json')
$hashes = [ordered]@{}
foreach ($path in $inputs) { $hashes[$path] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
$common = @('-std=c++17','-O2','-Wall','-Wextra','-Werror','-DUNICODE','-D_UNICODE','-DNOMINMAX','-static','-static-libgcc','-static-libstdc++','-Wl,--no-insert-timestamp')
$dllArguments = $common + @('-shared','-o',$dll,$source,$entry,'-lbcrypt')
$hostArguments = $common + @('-municode','-o',$hostExecutable,$hostSource)
$inspectorArguments = $common + @('-municode','-o',$inspector,$inspectorSource,'-luser32')
$oldCompilerPath = $env:Path
try {
    $env:Path = (Split-Path -Parent $Compiler) + ';' + $env:Path
    & $Compiler @dllArguments
    if ($LASTEXITCODE -ne 0) { throw "Diagnostic probe DLL build failed: $LASTEXITCODE" }
    & $Compiler @hostArguments
    if ($LASTEXITCODE -ne 0) { throw "Negative host build failed: $LASTEXITCODE" }
    & $Compiler @inspectorArguments
    if ($LASTEXITCODE -ne 0) { throw "Read-only inspector build failed: $LASTEXITCODE" }
} finally { $env:Path = $oldCompilerPath }
$negative = @()
if (-not $SkipNegativeTests) {
    foreach ($mode in @('--wrong-executable','--callback-refused')) {
        $raw = & $hostExecutable $dll $mode
        $code = $LASTEXITCODE
        $result = ($raw -join "`n") | ConvertFrom-Json
        $negative += [ordered]@{ command = @($hostExecutable,$dll,$mode); exit_code = $code; result = $result }
        Copy-Item -LiteralPath (Join-Path $output 'engine-dispatch-observation.json') -Destination (Join-Path $output ("negative-$($result.case)-observation.json")) -Force
        if ($code -ne 0 -or -not $result.passed) { throw "Negative probe host failed: $mode" }
    }
}
$report = [ordered]@{
    schema = 1; generated_utc = [DateTime]::UtcNow.ToString('o')
    diagnostic_only = $true; game_started = $false; installed = $false
    game_sha256 = (Get-FileHash -LiteralPath $game -Algorithm SHA256).Hash.ToLowerInvariant()
    expected_game_sha256 = (Get-SM2SupportedGameHash $game)
    source_sha256 = $hashes; compiler = $Compiler; dll_arguments = $dllArguments; host_arguments = $hostArguments
    dll = $dll; dll_sha256 = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant()
    host_sha256 = (Get-FileHash -LiteralPath $hostExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
    inspector_arguments = $inspectorArguments
    inspector_sha256 = (Get-FileHash -LiteralPath $inspector -Algorithm SHA256).Hash.ToLowerInvariant()
    negative_hosts = $negative
    limitation = 'Negative hosts prove refusal and no hook in unrelated executables. Real engine-thread observations require a separately coordinated game startup; no game was loaded or changed by this script.'
}
foreach ($path in $inputs) {
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hashes[$path]) { throw "Probe input changed during build/test: $path" }
}
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $output 'build-and-negative-tests.json') -Encoding UTF8
$report | ConvertTo-Json -Depth 12
