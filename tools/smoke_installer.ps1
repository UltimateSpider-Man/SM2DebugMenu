[CmdletBinding()]
param()

# Exercises the actual installer in isolated fixture folders. Never starts the
# game or invokes the generated launcher. Fixtures and backups remain reviewable.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$project = Split-Path -Parent $PSScriptRoot
$fixtureBase = Join-Path $project 'verification\installer-fixture'
$run = Join-Path $fixtureBase ([DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 6))
foreach ($relative in @('install.ps1', 'uninstall.ps1', 'info.json', 'dist\sm2debugmenu.dll', 'dist\SM2DebugMenu.script')) {
    if (-not (Test-Path -LiteralPath (Join-Path $project $relative) -PathType Leaf)) { throw "Required build artifact missing: $relative" }
}
$null = New-Item -ItemType Directory -Path $run -Force
$inputProject = Join-Path $run '_inputs\SM2DebugMenu'
$null = New-Item -ItemType Directory -Path $inputProject, (Join-Path $inputProject 'dist') -Force
foreach ($relative in @('install.ps1', 'uninstall.ps1', 'info.json', 'dist\sm2debugmenu.dll', 'dist\SM2DebugMenu.script')) {
    Copy-Item -LiteralPath (Join-Path $project $relative) -Destination (Join-Path $inputProject $relative)
}
$latin1 = [Text.Encoding]::GetEncoding(28591)
$results = New-Object 'Collections.Generic.List[object]'

function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function TextBytes([string]$Text) { return ,$latin1.GetBytes($Text) }
function AssertBytes([string]$Path, [byte[]]$Expected, [string]$Message) {
    Assert (Test-Path -LiteralPath $Path -PathType Leaf) "$Message (missing file: $Path)"
    $actual = [IO.File]::ReadAllBytes($Path)
    Assert ([Convert]::ToBase64String($actual) -ceq [Convert]::ToBase64String($Expected)) $Message
}
function New-Fixture([string]$Name, [byte[]]$Scripts) {
    $game = Join-Path $run $Name
    $localProject = Join-Path $game 'SM2DebugMenu'
    $null = New-Item -ItemType Directory -Path $game, $localProject, (Join-Path $localProject 'dist'),
        (Join-Path $game 'scripts'), (Join-Path $game 'Mods Library') -Force
    foreach ($relative in @('install.ps1', 'uninstall.ps1', 'info.json', 'dist\sm2debugmenu.dll', 'dist\SM2DebugMenu.script')) {
        Copy-Item -LiteralPath (Join-Path $inputProject $relative) -Destination (Join-Path $localProject $relative)
    }
    [IO.File]::WriteAllBytes((Join-Path $game 'Spider-Man2.exe'), (TextBytes 'FAKE GAME SENTINEL - NEVER EXECUTE'))
    [IO.File]::WriteAllBytes((Join-Path $game 'winmm.dll'), (TextBytes 'FAKE LOADER SENTINEL - NEVER LOAD'))
    [IO.File]::WriteAllBytes((Join-Path $game 'toc'), (TextBytes 'UNCHANGED TOC SENTINEL'))
    [IO.File]::WriteAllBytes((Join-Path $game 'scripts.txt'), $Scripts)
    [IO.File]::WriteAllBytes((Join-Path $game 'scripts\existing.dll'), (TextBytes 'UNCHANGED OTHER DLL'))
    [IO.File]::WriteAllBytes((Join-Path $game 'Mods Library\Unrelated.script'), (TextBytes 'UNCHANGED OTHER PACKAGE'))
    return [pscustomobject]@{ Game = $game; Project = $localProject; Scripts = $Scripts; Invocation = 0 }
}
function Invoke-FixtureScript($Fixture, [string]$Name) {
    $Fixture.Invocation++
    $log = Join-Path $Fixture.Game ("{0:00}-{1}.log" -f $Fixture.Invocation, $Name)
    & (Join-Path $Fixture.Project $Name) -GameDirectory $Fixture.Game *> $log
}
function Invoke-FixtureDefaultCli($Fixture, [string]$Name) {
    $Fixture.Invocation++
    $log = Join-Path $Fixture.Game ("{0:00}-default-cli-{1}.log" -f $Fixture.Invocation, $Name)
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Fixture.Project $Name) *> $log
    if ($LASTEXITCODE -ne 0) { throw "Default CLI $Name failed with exit code $LASTEXITCODE. See $log" }
}
function AssertSentinels($Fixture) {
    AssertBytes (Join-Path $Fixture.Game 'Spider-Man2.exe') (TextBytes 'FAKE GAME SENTINEL - NEVER EXECUTE') 'Installer changed the executable.'
    AssertBytes (Join-Path $Fixture.Game 'winmm.dll') (TextBytes 'FAKE LOADER SENTINEL - NEVER LOAD') 'Installer changed the loader.'
    AssertBytes (Join-Path $Fixture.Game 'toc') (TextBytes 'UNCHANGED TOC SENTINEL') 'Installer changed toc.'
    AssertBytes (Join-Path $Fixture.Game 'scripts\existing.dll') (TextBytes 'UNCHANGED OTHER DLL') 'Installer changed another script.'
    AssertBytes (Join-Path $Fixture.Game 'Mods Library\Unrelated.script') (TextBytes 'UNCHANGED OTHER PACKAGE') 'Installer changed another package.'
}
function AssertMenuRemoved($Fixture, [bool]$LauncherPreserved = $false) {
    foreach ($relative in @('scripts\sm2debugmenu.dll', 'Mods Library\SM2DebugMenu.script', 'SM2DebugMenu\install-state.json')) {
        Assert (-not (Test-Path -LiteralPath (Join-Path $Fixture.Game $relative))) "Uninstall left new file: $relative"
    }
    if (-not $LauncherPreserved) {
        Assert (-not (Test-Path -LiteralPath (Join-Path $Fixture.Game 'Launch SM2 Debug Menu.cmd'))) 'Uninstall left its unmodified launcher.'
    }
}
function Run-Case([string]$Name, [scriptblock]$Body) {
    try {
        & $Body
        $results.Add([pscustomobject]@{ Name = $Name; Passed = $true; Error = $null })
        Write-Host "PASS: $Name"
    } catch {
        $results.Add([pscustomobject]@{ Name = $Name; Passed = $false; Error = $_.Exception.Message })
        Write-Host "FAIL: $Name - $($_.Exception.Message)"
    }
}

Run-Case 'Repeated install; mixed newlines; no trailing newline; byte-exact uninstall' {
    $text = "modsettings.dll`r`nragesettings.dll`ncheatsmenu.dll"
    $fixture = New-Fixture '01 mixed line endings and spaces' (TextBytes $text)
    Invoke-FixtureScript $fixture 'install.ps1'
    $expected = TextBytes ($text + "`r`nsm2debugmenu.dll`r`n")
    AssertBytes (Join-Path $fixture.Game 'scripts.txt') $expected 'First installation changed existing script-list bytes.'
    foreach ($relative in @('scripts\sm2debugmenu.dll', 'Mods Library\SM2DebugMenu.script', 'Launch SM2 Debug Menu.cmd', 'SM2DebugMenu\install-state.json')) {
        Assert (Test-Path -LiteralPath (Join-Path $fixture.Game $relative) -PathType Leaf) "Install did not create $relative"
    }
    $launcher = "@echo off`r`ncd /d `"%~dp0`"`r`nif exist `"%~dp0Launch SM2 LTSC 2019.cmd`" (`r`n  call `"%~dp0Launch SM2 LTSC 2019.cmd`" %*`r`n) else (`r`n  start `"`" `"%~dp0Spider-Man2.exe`" -scripts %*`r`n)`r`n"
    AssertBytes (Join-Path $fixture.Game 'Launch SM2 Debug Menu.cmd') (TextBytes $launcher) 'Launcher must quote its paths, delegate LTSC startup, and forward arguments.'
    # Run only a harmless fixture stub, never the real game.
    $stub = "@echo off`r`n> `"%~dp0delegated-arguments.txt`" echo %*`r`n"
    [IO.File]::WriteAllBytes((Join-Path $fixture.Game 'Launch SM2 LTSC 2019.cmd'), (TextBytes $stub))
    $fixtureLaunch = Join-Path $fixture.Game 'Launch SM2 Debug Menu.cmd'
    $fixtureProcess = Start-Process -FilePath $env:ComSpec -ArgumentList @('/d','/c',('""' + $fixtureLaunch + '" --fixture "two words""')) -WindowStyle Hidden -PassThru -Wait
    Assert ($fixtureProcess.ExitCode -eq 0) 'Fixture launcher delegation failed.'
    Assert ((Get-Content -LiteralPath (Join-Path $fixture.Game 'delegated-arguments.txt') -Raw).Trim() -ceq '--fixture "two words"') 'LTSC launcher did not receive the original arguments.'
    Invoke-FixtureScript $fixture 'install.ps1'
    AssertBytes (Join-Path $fixture.Game 'scripts.txt') $expected 'Repeated install duplicated the menu or changed other entries.'
    $backups = @(Get-ChildItem -LiteralPath (Join-Path $fixture.Project 'backups') -Directory)
    Assert ($backups.Count -eq 2) 'Each installation must retain its own backup.'
    Invoke-FixtureScript $fixture 'uninstall.ps1'
    AssertBytes (Join-Path $fixture.Game 'scripts.txt') $fixture.Scripts 'Uninstall did not restore original script-list bytes.'
    AssertMenuRemoved $fixture
    Assert (@(Get-ChildItem -LiteralPath (Join-Path $fixture.Project 'backups') -Directory).Count -eq 2) 'Uninstall removed backups.'
    Invoke-FixtureScript $fixture 'uninstall.ps1'
    AssertBytes (Join-Path $fixture.Game 'scripts.txt') $fixture.Scripts 'Repeated uninstall changed existing scripts.'
    AssertSentinels $fixture
}

Run-Case 'Preserve an independently added script and a modified launcher' {
    $fixture = New-Fixture '02 preserve user changes' (TextBytes "modsettings.dll`r`ncheatsmenu.dll`r`n")
    Invoke-FixtureScript $fixture 'install.ps1'
    $list = Join-Path $fixture.Game 'scripts.txt'
    [IO.File]::AppendAllText($list, "new-user-script.dll`n", $latin1)
    $launcher = Join-Path $fixture.Game 'Launch SM2 Debug Menu.cmd'
    $customLauncher = TextBytes "@echo off`r`nrem User customization to preserve`r`n"
    [IO.File]::WriteAllBytes($launcher, $customLauncher)
    Invoke-FixtureScript $fixture 'uninstall.ps1'
    AssertBytes $list (TextBytes "modsettings.dll`r`ncheatsmenu.dll`r`nnew-user-script.dll`n") 'Uninstall damaged a script added after installation.'
    AssertBytes $launcher $customLauncher 'Uninstall deleted or replaced a modified launcher.'
    AssertMenuRemoved $fixture $true
    AssertSentinels $fixture
}

Run-Case 'Restore preexisting names after repeated installations' {
    $fixture = New-Fixture '03 restore preexisting files' (TextBytes 'existing.dll')
    $originals = @{
        'scripts\sm2debugmenu.dll' = (TextBytes 'PREEXISTING DLL BYTES')
        'Mods Library\SM2DebugMenu.script' = (TextBytes 'PREEXISTING PACKAGE BYTES')
        'Launch SM2 Debug Menu.cmd' = (TextBytes '@rem PREEXISTING LAUNCHER')
    }
    foreach ($relative in $originals.Keys) { [IO.File]::WriteAllBytes((Join-Path $fixture.Game $relative), $originals[$relative]) }
    Invoke-FixtureScript $fixture 'install.ps1'
    Invoke-FixtureScript $fixture 'install.ps1'
    Invoke-FixtureScript $fixture 'uninstall.ps1'
    foreach ($relative in $originals.Keys) {
        AssertBytes (Join-Path $fixture.Game $relative) $originals[$relative] "Uninstall did not restore preexisting file $relative"
    }
    AssertBytes (Join-Path $fixture.Game 'scripts.txt') $fixture.Scripts 'Uninstall failed to restore the initial script list.'
    Assert (-not (Test-Path -LiteralPath (Join-Path $fixture.Project 'install-state.json'))) 'Uninstall left installation state.'
    AssertSentinels $fixture
}

Run-Case 'Real filesystem copy failure rolls back a partially installed DLL' {
    $fixture = New-Fixture '04 filesystem rollback' (TextBytes "existing.dll`n")
    $dll = Join-Path $fixture.Game 'scripts\sm2debugmenu.dll'
    $package = Join-Path $fixture.Game 'Mods Library\SM2DebugMenu.script'
    $oldDll = TextBytes 'ORIGINAL DLL BEFORE FAILED INSTALL'
    $oldPackage = TextBytes 'ORIGINAL PACKAGE BEFORE FAILED INSTALL'
    [IO.File]::WriteAllBytes($dll, $oldDll)
    [IO.File]::WriteAllBytes($package, $oldPackage)
    # Lock the source package: validation and backups can finish; DLL copy can
    # succeed; the next package-copy operation receives a real sharing violation.
    $lock = [IO.File]::Open((Join-Path $fixture.Project 'dist\SM2DebugMenu.script'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
    $failure = $null
    try {
        try { Invoke-FixtureScript $fixture 'install.ps1' } catch { $failure = $_.Exception.Message }
    } finally { $lock.Dispose() }
    Assert ($null -ne $failure -and $failure -like '*previous files were restored*') "Expected a rollback error with recovery evidence, got: $failure"
    AssertBytes $dll $oldDll 'Rollback did not restore the DLL overwritten before copy failure.'
    AssertBytes $package $oldPackage 'Rollback did not preserve the old package.'
    AssertBytes (Join-Path $fixture.Game 'scripts.txt') $fixture.Scripts 'Rollback did not preserve the script list.'
    Assert (-not (Test-Path -LiteralPath (Join-Path $fixture.Game 'Launch SM2 Debug Menu.cmd'))) 'Failed install left a launcher.'
    Assert (-not (Test-Path -LiteralPath (Join-Path $fixture.Project 'install-state.json'))) 'Failed install left committed state.'
    Assert (@(Get-ChildItem -LiteralPath (Join-Path $fixture.Project 'backups') -Directory).Count -eq 1) 'Failed install should preserve its backup.'
    AssertSentinels $fixture
}

Run-Case 'Uninstall refuses to overwrite a changed DLL before touching other files' {
    $fixture = New-Fixture '05 changed DLL guard' (TextBytes 'existing.dll')
    Invoke-FixtureScript $fixture 'install.ps1'
    $dll = Join-Path $fixture.Game 'scripts\sm2debugmenu.dll'
    $changedDll = TextBytes 'USER REPLACED THE INSTALLED DLL'
    [IO.File]::WriteAllBytes($dll, $changedDll)
    $scriptBefore = [IO.File]::ReadAllBytes((Join-Path $fixture.Game 'scripts.txt'))
    $stateBefore = [IO.File]::ReadAllBytes((Join-Path $fixture.Project 'install-state.json'))
    $failure = $null
    try { Invoke-FixtureScript $fixture 'uninstall.ps1' } catch { $failure = $_.Exception.Message }
    Assert ($null -ne $failure -and $failure -like '*No files were changed*') "Expected a changed-DLL guard, got: $failure"
    AssertBytes $dll $changedDll 'Guarded uninstall changed the user-replaced DLL.'
    AssertBytes (Join-Path $fixture.Game 'scripts.txt') $scriptBefore 'Guarded uninstall changed scripts.txt.'
    AssertBytes (Join-Path $fixture.Project 'install-state.json') $stateBefore 'Guarded uninstall changed its state.'
    Copy-Item -LiteralPath (Join-Path $fixture.Project 'dist\sm2debugmenu.dll') -Destination $dll -Force
    Invoke-FixtureScript $fixture 'uninstall.ps1'
    AssertBytes (Join-Path $fixture.Game 'scripts.txt') $fixture.Scripts 'Recovery uninstall damaged the original script list.'
    AssertMenuRemoved $fixture
    AssertSentinels $fixture
}

Run-Case 'CLI defaults locate the fixture game when GameDirectory is omitted' {
    $fixture = New-Fixture '06 default CLI folder with spaces' (TextBytes 'existing.dll')
    Invoke-FixtureDefaultCli $fixture 'install.ps1'
    AssertBytes (Join-Path $fixture.Game 'scripts.txt') (TextBytes "existing.dll`r`nsm2debugmenu.dll`r`n") 'Default CLI install did not update the fixture parent.'
    foreach ($relative in @('scripts\sm2debugmenu.dll', 'Mods Library\SM2DebugMenu.script', 'Launch SM2 Debug Menu.cmd')) {
        Assert (Test-Path -LiteralPath (Join-Path $fixture.Game $relative) -PathType Leaf) "Default CLI install missed $relative"
    }
    $state = Get-Content -LiteralPath (Join-Path $fixture.Project 'install-state.json') -Raw | ConvertFrom-Json
    Assert ([String]::Equals($state.GameDirectory, $fixture.Game, [StringComparison]::OrdinalIgnoreCase)) 'Default CLI install recorded the wrong game directory.'
    Invoke-FixtureDefaultCli $fixture 'uninstall.ps1'
    AssertBytes (Join-Path $fixture.Game 'scripts.txt') $fixture.Scripts 'Default CLI uninstall did not restore original script-list bytes.'
    AssertMenuRemoved $fixture
    AssertSentinels $fixture
}

$report = [pscustomobject]@{
    CreatedUtc = [DateTime]::UtcNow.ToString('o')
    FixtureRoot = $run
    NoGameStarted = $true
    Sources = @('install.ps1', 'uninstall.ps1', 'dist\sm2debugmenu.dll', 'dist\SM2DebugMenu.script') | ForEach-Object {
        $path = Join-Path $inputProject $_
        [pscustomobject]@{ Path = (Join-Path $project $_); TestedCopy = $path; Sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
    }
    Results = @($results.ToArray())
}
$reportPath = Join-Path $run 'results.json'
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $reportPath -Encoding UTF8
Write-Host "Installer verification report: $reportPath"
if (@($results | Where-Object { -not $_.Passed }).Count -gt 0) { exit 1 }
