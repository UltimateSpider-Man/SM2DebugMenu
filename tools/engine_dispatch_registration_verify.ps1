[CmdletBinding()]
param([string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Split-Path -Parent $PSScriptRoot
$output = Join-Path $project 'verification\engine-dispatch-registration'
$null = [IO.Directory]::CreateDirectory($output)
$source = Join-Path $project 'src\engine_dispatch.cpp'
$header = Join-Path $project 'src\engine_dispatch.h'
$fixture = Join-Path $PSScriptRoot 'engine_dispatch_registration_test.cpp'
$executable = Join-Path $output 'engine_dispatch_registration_test.exe'
$frozenDll = Join-Path $project 'verification\engine-dispatch-probe\engine_dispatch_probe.dll'
$frozenHash = '2d15f199bfa721b41ae4ef9328fce020f0f09f97572362077e1cb7dae78156ad'
if ((Get-FileHash -LiteralPath $frozenDll -Algorithm SHA256).Hash.ToLowerInvariant() -cne $frozenHash) { throw 'Reviewed diagnostic DLL changed.' }
$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$headerHash = (Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash.ToLowerInvariant()
$identityHeader = Join-Path (Join-Path $project 'src') 'supported_builds.h'
$identityHash = (Get-FileHash -LiteralPath $identityHeader -Algorithm SHA256).Hash.ToLowerInvariant()
$compileArguments = @('-std=c++17','-O2','-Wall','-Wextra','-Werror','-DUNICODE','-D_UNICODE','-DNOMINMAX',
    '-static','-static-libgcc','-static-libstdc++','-Wl,--no-insert-timestamp','-o',$executable,$fixture,'-lbcrypt')
$oldCompilerPath = $env:Path
try {
    $env:Path = (Split-Path -Parent $Compiler) + ';' + $env:Path
    & $Compiler @compileArguments
    if ($LASTEXITCODE -ne 0) { throw "Registration fixture build failed: $LASTEXITCODE" }
    $raw = & $executable
    $code = $LASTEXITCODE
    $result = ($raw -join "`n") | ConvertFrom-Json
} finally { $env:Path = $oldCompilerPath }
if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -cne $sourceHash -or
    (Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash.ToLowerInvariant() -cne $headerHash -or
    (Get-FileHash -LiteralPath $identityHeader -Algorithm SHA256).Hash.ToLowerInvariant() -cne $identityHash) { throw 'Dispatcher changed during verification.' }
if ((Get-FileHash -LiteralPath $frozenDll -Algorithm SHA256).Hash.ToLowerInvariant() -cne $frozenHash) { throw 'Reviewed diagnostic DLL changed during tests.' }
$report = [ordered]@{
    schema = 1; generated_utc = [DateTime]::UtcNow.ToString('o'); source_sha256 = $sourceHash; header_sha256 = $headerHash
    supported_builds_header_sha256 = $identityHash
    fixture_sha256 = (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash.ToLowerInvariant()
    executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
    frozen_diagnostic_dll_sha256 = $frozenHash; compiler = $Compiler; compile_arguments = $compileArguments
    command = $executable; exit_code = $code; result = $result
}
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $output 'registration-tests.json') -Encoding UTF8
$report | ConvertTo-Json -Depth 12
if ($code -ne 0 -or -not $result.passed) { throw 'Registration lifecycle verification failed.' }
