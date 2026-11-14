# Exact identities from the same self-contained header compiled into the mod.
# This is source policy, not a user-supplied manifest or normalized fingerprint.
$script:Sm2BuildPolicyHeader = Join-Path (Split-Path -Parent $PSScriptRoot) 'src\supported_builds.h'
$script:Sm2BuildPolicyText = [IO.File]::ReadAllText($script:Sm2BuildPolicyHeader)
$script:Sm2BuildHashes = @()
foreach ($sm2IdentityName in @('kOriginalHash','kBootstrapHash')) {
    $sm2IdentityMatches = [regex]::Matches($script:Sm2BuildPolicyText, ('inline constexpr char\s+' + $sm2IdentityName + '\[\]\s*=\s*"([0-9a-f]{64})"\s*;'))
    if ($sm2IdentityMatches.Count -ne 1) { throw "Missing or ambiguous compiled executable identity: $sm2IdentityName" }
    $script:Sm2BuildHashes += $sm2IdentityMatches[0].Groups[1].Value
}
if ($script:Sm2BuildHashes[0] -ceq $script:Sm2BuildHashes[1]) { throw 'Canonical and bootstrap executable identities must differ.' }
function Test-SM2SupportedGameHash([string]$Hash) {
    return $Hash -cin $script:Sm2BuildHashes
}
function Get-SM2SupportedGameHash([string]$Path) {
    $sm2ActualHash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if (-not (Test-SM2SupportedGameHash $sm2ActualHash)) {
        throw 'Unsupported executable: expected the canonical build or exact approved bootstrap variant.'
    }
    return $sm2ActualHash
}
