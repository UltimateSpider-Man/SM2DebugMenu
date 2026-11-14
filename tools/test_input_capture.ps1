[CmdletBinding()]
param([string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Split-Path -Parent $PSScriptRoot
$inputs = @('src\game_input_capture.cpp','src\game_input_capture.h','src\controller_input.cpp','src\controller_input.h','src\overlay.h','tools\input_capture_fixture.cpp') |
    ForEach-Object { Join-Path $project $_ }
$hashes = [ordered]@{}
foreach ($path in $inputs) { $hashes[$path] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
$executable = Join-Path $project 'verification\input_capture_fixture.exe'
$arguments = @('-std=c++17','-O2','-Wall','-Wextra','-Werror','-DUNICODE','-D_UNICODE','-DNOMINMAX',
    '-static','-static-libgcc','-static-libstdc++','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
    '-o',$executable,(Join-Path $PSScriptRoot 'input_capture_fixture.cpp'),'-luser32','-lhid','-lsetupapi')
$savedCompilerPath = $env:Path
try {
    $env:Path = (Split-Path -Parent $Compiler) + ';' + $env:Path
    & $Compiler @arguments
    if ($LASTEXITCODE -ne 0) { throw "Input capture fixture compilation failed: $LASTEXITCODE" }
    $raw = & $executable
    $testExit = $LASTEXITCODE
    $result = ($raw -join "`n") | ConvertFrom-Json
} finally { $env:Path = $savedCompilerPath }
foreach ($path in $inputs) {
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hashes[$path]) {
        throw "Input changed during verification: $path"
    }
}
$report = [ordered]@{
    schema = 1
    generated_utc = [DateTime]::UtcNow.ToString('o')
    input_sha256 = $hashes
    compiler = $Compiler
    compile_arguments = $arguments
    executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
    command = $executable
    exit_code = $testExit
    scope = 'Real production protocol and input filters; emulated API completions/window forwarding; no production import patch, game, GUI or physical input.'
    result = $result
}
$destination = Join-Path $project 'verification\input-capture-tests.json'
[IO.File]::WriteAllText($destination, ($report | ConvertTo-Json -Depth 12), (New-Object Text.UTF8Encoding $false))
$report | ConvertTo-Json -Depth 12
if ($testExit -ne 0 -or -not $result.passed) { throw "Input capture tests failed: $destination" }
