[CmdletBinding()]
param([switch]$IncludeRelease)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Split-Path -Parent $PSScriptRoot
$verification = Join-Path $project 'verification'
$names = @(
    'reload-runtime-fixture\reload-runtime-tests.json',
    'primary-objective-fixture\primary-objective-tests.json',
    'actor-save-runtime-tests.json',
    'replay-runtime-fixture\replay-runtime-tests.json',
    'hero-actions-fixture\hero-actions-tests.json',
    'menu-model-tests.json',
    'input-capture-tests.json',
    'photo-gate-tests.json'
)
$checks = @()
foreach ($name in $names) {
    $path = Join-Path $verification $name
    $report = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if ($report.exit_code -ne 0 -or $report.result.passed -isnot [bool] -or -not $report.result.passed) {
        throw "Verification did not pass: $name"
    }
    $inputs = @($report.input_sha256.PSObject.Properties)
    if ($inputs.Count -eq 0) { throw "Verification has no source fingerprints: $name" }
    foreach ($fingerprint in $inputs) {
        $expected = [string]$fingerprint.Value
        if ($expected -notmatch '^[0-9a-fA-F]{64}$' -or
            (Get-FileHash -LiteralPath $fingerprint.Name -Algorithm SHA256).Hash -ine $expected) {
            throw "Stale verification input in ${name}: $($fingerprint.Name)"
        }
    }
    $cases = if ($report.result.PSObject.Properties['cases']) {
        if ($report.result.cases -is [Array]) { $report.result.cases.Count } else { [int]$report.result.cases }
    } else { @($report.result.tests).Count }
    $checks += [pscustomobject]@{
        report = $name
        report_sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        current_input_count = $inputs.Count
        passed = $true
        cases = $cases
    }
}
$photoProofPath = Join-Path $verification 'photo-gate-native-proof.json'
$photoProof = Get-Content -LiteralPath $photoProofPath -Raw | ConvertFrom-Json
if (-not $photoProof.passed -or
    (Get-FileHash -LiteralPath (Join-Path $project 'src\photo_mode_gate.cpp') -Algorithm SHA256).Hash -ine $photoProof.source_sha256 -or
    (Get-FileHash -LiteralPath (Join-Path $project 'src\photo_mode_gate.h') -Algorithm SHA256).Hash -ine $photoProof.header_sha256 -or
    (Get-FileHash -LiteralPath (Join-Path (Split-Path -Parent $project) 'Spider-Man2.exe') -Algorithm SHA256).Hash -ine $photoProof.exe_sha256) {
    throw 'Native photo gate proof is missing, stale or failed.'
}
if ($IncludeRelease) {
    $path = Join-Path $verification 'release-package-tests.json'
    $release = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if (-not $release.passed -or -not $release.package_dll_matches -or -not $release.imports_resolved_on_this_os -or
        $release.version -cne (Get-Content -LiteralPath (Join-Path $project 'info.json') -Raw | ConvertFrom-Json).version) {
        throw 'Release loader/package verification is missing or failed.'
    }
    foreach ($artifact in @(
        @{ Name='sm2debugmenu.dll'; Hash=$release.dll_sha256 },
        @{ Name='SM2DebugMenu.script'; Hash=$release.package_sha256 },
        @{ Name='SM2DebugMenuPreview.exe'; Hash=$release.preview_sha256 }
    )) {
        if ((Get-FileHash -LiteralPath (Join-Path (Join-Path $project 'dist') $artifact.Name) -Algorithm SHA256).Hash -ine $artifact.Hash) {
            throw "Release artifact changed after verification: $($artifact.Name)"
        }
    }
}
$result = [ordered]@{
    schema = 1
    generated_utc = [DateTime]::UtcNow.ToString('o')
    passed = $true
    release_included = [bool]$IncludeRelease
    checks = $checks
    total_cases = [int](($checks | Measure-Object -Property cases -Sum).Sum)
    live_game_actions_verified = $false
    scope = 'Fresh source fingerprints and existing synthetic checks; no game actions executed'
}
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $verification 'reload-verification-gate.json') -Encoding UTF8
$result | ConvertTo-Json -Depth 6
