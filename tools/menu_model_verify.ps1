[CmdletBinding()]
param([string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Split-Path -Parent $PSScriptRoot
$build = Join-Path $project 'verification\build'
$null = [IO.Directory]::CreateDirectory($build)
$executable = Join-Path $build 'menu_model_test.exe'
$reportPath = Join-Path $project 'verification\menu-model-tests.json'
$imgui = Join-Path $project 'vendor\imgui'
$testSource = Join-Path $PSScriptRoot 'menu_model_test.cpp'
$uiSource = Join-Path $project 'src\menu_ui.cpp'
$catalogSource = Join-Path $project 'src\reference_catalog.cpp'
$nativeCatalogSource = Join-Path $project 'src\native_dev_catalog.cpp'
$reloadSource = Join-Path $project 'src\reload_runtime.cpp'
$missionCatalogSource = Join-Path $project 'src\reload_mission_catalog.cpp'
$bindingContract = Join-Path $project 'verification\retail-reimplementation\gameplay-binding-contract.json'
$customBindingContract = Join-Path $project 'verification\full-reimplementation-progress\gameplay-wave2-contract.json'
$rawBindingContract = Join-Path $project 'verification\full-reimplementation-progress\gameplay-reactions-contract.json'
$bindingManifest = Join-Path $project 'verification\debug-settings-bindings.json'
$bindingRows = @((Get-Content -LiteralPath $bindingContract -Raw | ConvertFrom-Json).rows) + @((Get-Content -LiteralPath $customBindingContract -Raw | ConvertFrom-Json).rows) + @((Get-Content -LiteralPath $rawBindingContract -Raw | ConvertFrom-Json).rows)
if ($bindingRows.Count -ne 39) { throw 'Unexpected source port setting contract row count' }
$manifest = Get-Content -LiteralPath $bindingManifest -Raw | ConvertFrom-Json
$realBindings = @($manifest.bindings) + @($manifest.raw_booleans)
$fixtureFields = [regex]::Matches((Get-Content -LiteralPath $testSource -Raw), '^\s*\{"([^"]+)","([^"]+)","([^"]+)",DebugSettingType::(\w+)\},?\s*$', 'Multiline')
foreach ($row in $bindingRows) {
    $fixtureMatchRows = @($fixtureFields | Where-Object { $_.Groups[1].Value -eq $row.id -and $_.Groups[2].Value -eq $row.page -and $_.Groups[3].Value -ceq $row.label })
    $typed = @($realBindings | Where-Object { $_.id -eq $row.id })
    if ($fixtureMatchRows.Count -ne 1 -or $typed.Count -ne 1 -or $fixtureMatchRows[0].Groups[4].Value -ne $typed[0].type) {
        throw "UI fixture metadata/type differs from generated binding: $($row.id) / $($row.page) / $($row.label)"
    }
}
$sources = @($testSource, $catalogSource, $nativeCatalogSource, $reloadSource, $missionCatalogSource) + @('imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp' | ForEach-Object { Join-Path $imgui $_ })
$inputs = @($uiSource,$bindingContract,$customBindingContract,$rawBindingContract,$bindingManifest,$PSCommandPath) + $sources + @('debugmenu.h','debug_settings.h','reload_runtime.h','reload_options_runtime.h','reload_selection_runtime.h','primary_objective_runtime.h','actor_save_runtime.h','replay_runtime.h','reload_mission_catalog.h','engine_dispatch.h','overlay.h','reference_catalog.h','native_dev_catalog.h','screen_capture.h','camera_debug_runtime.h' | ForEach-Object { Join-Path (Join-Path $project 'src') $_ }) + @('imgui.h','imgui_internal.h','imconfig.h' | ForEach-Object { Join-Path $imgui $_ })
foreach ($path in @($Compiler) + $inputs) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required test input missing: $path" }
}
$before = @{}
foreach ($path in $inputs) { $before[$path] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
$compileArguments = @('-std=c++17','-O2','-Wall','-Wextra','-Werror','-DUNICODE','-D_UNICODE','-DNOMINMAX','-ffunction-sections','-fdata-sections',
    '-static','-static-libgcc','-static-libstdc++','-I',$imgui,'-Wl,--gc-sections','-o',$executable) + $sources + @('-luser32')
$previousPath = $env:Path
try {
    $env:Path = (Split-Path -Parent $Compiler) + ';' + $env:Path
    & $Compiler @compileArguments
    if ($LASTEXITCODE -ne 0) { throw "Menu model test compilation failed: $LASTEXITCODE" }
    $raw = & $executable
    $testExit = $LASTEXITCODE
    $result = ($raw -join "`n") | ConvertFrom-Json
    foreach ($path in $before.Keys) {
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $before[$path]) { throw "Test input changed during execution: $path. Rerun against frozen sources." }
    }
    $report = [ordered]@{
        schema = 1
        generated_utc = [DateTime]::UtcNow.ToString('o')
        ui_source_sha256 = $before[$uiSource]
        catalog_source_sha256 = $before[$catalogSource]
        test_source_sha256 = $before[$testSource]
        input_sha256 = $before
        reload_context_comparison = 'production SameReloadContext; engine context stub always false and asserted unused'
        source_port_setting_rows = $bindingRows.Count
        source_port_binding_contract_sha256 = $before[$bindingContract]
        custom_binding_contract_sha256 = $before[$customBindingContract]
        raw_binding_contract_sha256 = $before[$rawBindingContract]
        executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
        command = $executable
        compiler = $Compiler
        compile_arguments = $compileArguments
        exit_code = $testExit
        game_launched = $false
        windows_created = $false
        gpu_or_hid_used = $false
        result = $result
    }
    [IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 12), (New-Object Text.UTF8Encoding $false))
    Write-Output ($report | ConvertTo-Json -Depth 12)
    if ($testExit -ne 0 -or -not $result.passed) { throw "Menu model verification failed; inspect $reportPath" }
} finally { $env:Path = $previousPath }
