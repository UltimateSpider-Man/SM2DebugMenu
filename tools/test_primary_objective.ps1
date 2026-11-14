[CmdletBinding()]
param([string]$Compiler='C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$project=Split-Path -Parent $PSScriptRoot
$game=Join-Path (Split-Path -Parent $project) 'Spider-Man2.exe'
. (Join-Path $PSScriptRoot 'supported_builds.ps1')
$gameHash = Get-SM2SupportedGameHash $game
$output=Join-Path $project 'verification\primary-objective-fixture'
$null=[IO.Directory]::CreateDirectory($output)
$fixture=Join-Path $PSScriptRoot 'primary_objective_fixture.cpp'
$inputs=@($fixture)+( 'primary_objective_runtime.cpp','primary_objective_runtime.h','reload_runtime.h','engine_dispatch.h' | ForEach-Object {Join-Path (Join-Path $project 'src') $_})
$inputs += @((Join-Path $PSScriptRoot 'supported_builds.ps1'), (Join-Path (Join-Path $project 'src') 'supported_builds.h'))
$hashes=[ordered]@{}
foreach($path in $inputs){$hashes[$path]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
$exe=Join-Path $output 'primary_objective_fixture.exe'
$arguments=@('-std=c++17','-O2','-Wall','-Wextra','-Werror','-DUNICODE','-D_UNICODE','-DNOMINMAX','-municode','-static','-static-libgcc','-static-libstdc++','-Wl,--no-insert-timestamp',$fixture,'-o',$exe,'-luser32')
$oldPath=$env:Path
try{
 $env:Path=(Split-Path -Parent $Compiler)+';'+$env:Path
 & $Compiler @arguments
 if($LASTEXITCODE -ne 0){throw "Primary fixture compilation failed: $LASTEXITCODE"}
 $raw=& $exe $game
 $exitCode=$LASTEXITCODE
 $result=($raw -join "`n") | ConvertFrom-Json
}finally{$env:Path=$oldPath}
foreach($path in $inputs){if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hashes[$path]){throw "Fixture input changed: $path"}}
$report=[ordered]@{schema=1;generated_utc=[DateTime]::UtcNow.ToString('o');input_sha256=$hashes;game_file_sha256=$gameHash;compiler=$Compiler;compile_arguments=$arguments;command=@($exe,$game);exit_code=$exitCode;executable_sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant();game_launched=$false;real_engine_context_proven=$false;result=$result}
$reportPath=Join-Path $output 'primary-objective-tests.json'
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $reportPath -Encoding UTF8
$report | ConvertTo-Json -Depth 12
if($exitCode -ne 0 -or -not $result.passed){throw "Primary objective fixture failed; inspect $reportPath"}
