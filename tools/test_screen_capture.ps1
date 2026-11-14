[CmdletBinding()]
param([string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Split-Path -Parent $PSScriptRoot
$output = Join-Path $project 'verification\screen-capture-fixture'
$null = [IO.Directory]::CreateDirectory($output)
$inputs = @('src\screen_capture.cpp','src\screen_capture.h','tools\screen_capture_fixture.cpp')
$hashes = @{}
foreach ($inputFile in $inputs) { $hashes[$inputFile] = (Get-FileHash -LiteralPath (Join-Path $project $inputFile) -Algorithm SHA256).Hash.ToLowerInvariant() }
$priorPath = $env:Path
try {
    $env:Path = (Split-Path -Parent $Compiler) + ';' + $env:Path
    $exe = Join-Path $output 'screen_capture_fixture.exe'
    & $Compiler -std=c++17 -O2 -Wall -Wextra -Werror -DUNICODE -D_UNICODE -DNOMINMAX -municode -static -static-libgcc -static-libstdc++ (Join-Path $project 'src\screen_capture.cpp') (Join-Path $project 'tools\screen_capture_fixture.cpp') -o $exe -lole32 -luuid -lwindowscodecs
    if ($LASTEXITCODE -ne 0) { throw 'Capture fixture compilation failed' }
    $imageDirectory = Join-Path $output ([Guid]::NewGuid().ToString('N'))
    $raw = & $exe $imageDirectory
    $fixtureExit = $LASTEXITCODE
    if ($fixtureExit -ne 0) { throw "Capture fixture failed: $fixtureExit" }
    $result = ($raw -join "`n") | ConvertFrom-Json
    if (-not $result.passed) { throw 'Capture fixture did not pass' }
    foreach ($inputFile in $inputs) {
        if ((Get-FileHash -LiteralPath (Join-Path $project $inputFile) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hashes[$inputFile]) { throw "Capture input changed: $inputFile" }
    }
    $report = [ordered]@{ generated_utc = [DateTime]::UtcNow.ToString('o'); input_sha256 = $hashes; executable_sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant(); result = $result; images = $imageDirectory; exit_code = $fixtureExit }
    [IO.File]::WriteAllText((Join-Path $output 'screen-capture-tests.json'),($report | ConvertTo-Json -Depth 8),[Text.Encoding]::UTF8)
    $report | ConvertTo-Json -Depth 8
} finally { $env:Path = $priorPath }
