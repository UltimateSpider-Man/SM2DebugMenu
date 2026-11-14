[CmdletBinding()]
param([string]$GameExe = '', [string]$OutputPath = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'supported_builds.ps1')
if (-not $GameExe) { $GameExe = Join-Path (Split-Path -Parent $project) 'Spider-Man2.exe' }
if (-not $OutputPath) { $OutputPath = Join-Path $project 'verification\engine-dispatch-probe\static-binding-proof.json' }
$null = [IO.Directory]::CreateDirectory((Split-Path -Parent $OutputPath))
$file = [IO.File]::Open($GameExe, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
try {
    function Read-Range([long]$Offset, [int]$Count) {
        $buffer = New-Object byte[] $Count
        $file.Position = $Offset
        if ($file.Read($buffer, 0, $Count) -ne $Count) { throw 'Truncated PE input.' }
        return ,$buffer
    }
    function U32([long]$Offset) { [BitConverter]::ToUInt32((Read-Range $Offset 4), 0) }
    function U64([long]$Offset) { [BitConverter]::ToUInt64((Read-Range $Offset 8), 0) }
    $peOffset = U32 0x3c
    $optional = $peOffset + 24
    $sectionCount = [BitConverter]::ToUInt16((Read-Range ($peOffset + 6) 2), 0)
    $sectionTable = $optional + [BitConverter]::ToUInt16((Read-Range ($peOffset + 20) 2), 0)
    $sections = @()
    for ($sectionIndex = 0; $sectionIndex -lt $sectionCount; ++$sectionIndex) {
        $entry = Read-Range ($sectionTable + $sectionIndex * 40) 40
        $sections += [pscustomobject]@{ name = [Text.Encoding]::ASCII.GetString($entry, 0, 8).Trim([char]0)
            rva = [BitConverter]::ToUInt32($entry, 12); raw_size = [BitConverter]::ToUInt32($entry, 16)
            raw = [BitConverter]::ToUInt32($entry, 20); flags = [BitConverter]::ToUInt32($entry, 36) }
    }
    function Offset-ForRva([uint32]$Rva) {
        foreach ($section in $sections) {
            if ($Rva -ge $section.rva -and $Rva -lt $section.rva + $section.raw_size) { return [long]($Rva - $section.rva + $section.raw) }
        }
        throw ('No file bytes for RVA0x{0:x}.' -f $Rva)
    }
    $exceptionRva = U32 ($optional + 112 + 3 * 8)
    $exceptionBytes = U32 ($optional + 116 + 3 * 8)
    $exceptionOffset = Offset-ForRva $exceptionRva
    function Find-Function([uint32]$Rva) {
        $low = 0; $high = [int]($exceptionBytes / 12) - 1
        while ($low -le $high) {
            $middle = [int][Math]::Floor(($low + $high) / 2)
            $record = Read-Range ($exceptionOffset + $middle * 12) 12
            $begin = [BitConverter]::ToUInt32($record, 0); $end = [BitConverter]::ToUInt32($record, 4)
            if ($Rva -lt $begin) { $high = $middle - 1 }
            elseif ($Rva -ge $end) { $low = $middle + 1 }
            else { return [pscustomobject]@{ begin = $begin; end = $end } }
        }
        return $null
    }
    $checks = @()
    $actualHash = (Get-FileHash -LiteralPath $GameExe -Algorithm SHA256).Hash.ToLowerInvariant()
    $checks += [ordered]@{ name = 'known_file_hash'; passed = (Test-SM2SupportedGameHash $actualHash); actual = $actualHash }
    $checks += [ordered]@{ name = 'known_pe_identity'; passed = (U32 ($peOffset + 8)) -eq 0x679b5991 -and (U32 ($optional + 56)) -eq 0xce54000 -and (U64 ($optional + 24)) -eq 0x140000000 }
    foreach ($check in @(
        @{ name = 'original_leaf_and_padding'; rva = 0x373b20; expected = 'c20000cccccccccccccccccccccccccc' },
        @{ name = 'exact_virtual_call_and_return_site'; rva = 0x2af6a31; expected = '488b07488bcfff5020488d0dff685c09' },
        @{ name = 'actual_game_tls_main_role_checker'; rva = 0x3083b70; expected = '8b0d7a612c0965488b042558000000bab0000000488b04c88b0c02390d0751f9060f94c0c3' }
    )) {
        $bytes = Read-Range (Offset-ForRva $check.rva) ($check.expected.Length / 2)
        $actual = [BitConverter]::ToString($bytes).Replace('-', '').ToLowerInvariant()
        $checks += [ordered]@{ name = $check.name; passed = $actual -ceq $check.expected; rva = $check.rva; actual = $actual }
    }
    $frame = Find-Function 0x2af6a3a
    $leaf = Find-Function 0x373b20
    $checks += [ordered]@{ name = 'frame_unwind_and_original_no_unwind_leaf'; passed = $null -ne $frame -and $frame.begin -eq 0x2af6620 -and $frame.end -eq 0x2af76f5 -and $null -eq $leaf; actual = $frame }
    $tlsRva = U32 ($optional + 112 + 9 * 8)
    $addressOfIndex = U64 ((Offset-ForRva $tlsRva) + 16)
    $checks += [ordered]@{ name = 'pe_tls_index_matches_disassembled_global'; passed = $addressOfIndex -eq 0x14c349cf0; actual = $addressOfIndex }
    $expectedMethods = @(0x140373c80, 0x140373cb0, 0x143507910, 0x143507a00, 0x140373b20)
    $actualMethods = @(); $matchingMethods = $true
    for ($methodIndex = 0; $methodIndex -lt 5; ++$methodIndex) {
        $method = U64 ((Offset-ForRva 0x52f4e58) + $methodIndex * 8)
        $actualMethods += $method
        if ($method -ne $expectedMethods[$methodIndex]) { $matchingMethods = $false }
    }
    $checks += [ordered]@{ name = 'game_mode_control_primary_vtable'; passed = $matchingMethods; actual = $actualMethods }
    $passed = @($checks | Where-Object { -not $_.passed }).Count -eq 0
    $report = [ordered]@{ schema = 1; generated_utc = [DateTime]::UtcNow.ToString('o'); read_only = $true; game_loaded = $false; passed = $passed; game = $GameExe; checks = $checks }
    $report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
    if (-not $passed) { throw "Static dispatcher binding proof failed; inspect $OutputPath" }
    Write-Host ('Static dispatcher binding proof: PASS ({0} checks)' -f $checks.Count)
} finally { $file.Dispose() }
