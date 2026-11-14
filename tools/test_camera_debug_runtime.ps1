[CmdletBinding()]
param([string]$Compiler='C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$project=Split-Path -Parent $PSScriptRoot
$game=Join-Path (Split-Path -Parent $project) 'Spider-Man2.exe'
. (Join-Path $PSScriptRoot 'supported_builds.ps1')
$gameHash = Get-SM2SupportedGameHash $game
$output=Join-Path $project 'verification\camera-debug-fixture'
$null=[IO.Directory]::CreateDirectory($output)
$fixture=Join-Path $PSScriptRoot 'camera_debug_runtime_fixture.cpp'
$inputs=@($fixture,(Join-Path $PSScriptRoot 'reload_runtime_fixture.cpp'),$PSCommandPath)+(
 'camera_debug_runtime.cpp','camera_debug_runtime.h','reload_runtime.cpp','reload_runtime.h','engine_dispatch.h' |
 ForEach-Object {Join-Path (Join-Path $project 'src') $_})
$inputs += @((Join-Path $PSScriptRoot 'supported_builds.ps1'), (Join-Path (Join-Path $project 'src') 'supported_builds.h'))
$hashes=[ordered]@{}
foreach($path in $inputs){$hashes[$path]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
$exe=Join-Path $output 'camera_debug_runtime_fixture.exe'
$arguments=@('-std=c++17','-O2','-Wall','-Wextra','-Werror','-DUNICODE','-D_UNICODE','-DNOMINMAX','-municode',
 '-static','-static-libgcc','-static-libstdc++','-Wl,--no-insert-timestamp',$fixture,'-o',$exe,'-luser32')
$oldPath=$env:Path
try{
 $env:Path=(Split-Path -Parent $Compiler)+';'+$env:Path
 & $Compiler @arguments
 if($LASTEXITCODE -ne 0){throw "Camera fixture compilation failed: $LASTEXITCODE"}
 $startInfo=New-Object Diagnostics.ProcessStartInfo
 $startInfo.FileName=$exe
 $startInfo.Arguments='"'+$game+'"'
 $startInfo.UseShellExecute=$false
 $startInfo.CreateNoWindow=$true
 $startInfo.RedirectStandardOutput=$true
 $startInfo.RedirectStandardError=$true
 $process=New-Object Diagnostics.Process
 $process.StartInfo=$startInfo
 $null=$process.Start()
 $standardOutput=$process.StandardOutput.ReadToEndAsync()
 $standardError=$process.StandardError.ReadToEndAsync()
 if(-not $process.WaitForExit(30000)){$process.Kill();$process.WaitForExit();throw 'Camera fixture timed out'}
 $raw=$standardOutput.Result
 $errors=$standardError.Result
 $exitCode=$process.ExitCode
 $process.Dispose()
 $raw | Set-Content -LiteralPath (Join-Path $output 'stdout.log') -Encoding UTF8
 $errors | Set-Content -LiteralPath (Join-Path $output 'stderr.log') -Encoding UTF8
 if($errors){Write-Host $errors}
 if($exitCode -ne 0){throw "Camera fixture failed: $exitCode"}
 $result=($raw -join "`n") | ConvertFrom-Json
}finally{$env:Path=$oldPath}
foreach($path in $inputs){if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hashes[$path]){throw "Fixture input changed: $path"}}
$report=[ordered]@{schema=1;generated_utc=[DateTime]::UtcNow.ToString('o');input_sha256=$hashes;game_file_sha256=$gameHash;
 compiler=$Compiler;compile_arguments=$arguments;command=@($exe,$game);exit_code=$exitCode;
 executable_sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant();
 scope='Actual retail PE pins and production camera shake reader/writer/conditional restoration; synthetic owned camera and world memory; no native game calls or live process access.';result=$result}
$destination=Join-Path $output 'camera-debug-tests.json'
$report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $destination -Encoding UTF8
$report | ConvertTo-Json -Depth 12
if(-not $result.passed){throw "Camera fixture failed: $destination"}
