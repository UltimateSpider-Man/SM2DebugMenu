[CmdletBinding()]
param(
    [string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe',
    [switch]$SkipPreview
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path -LiteralPath $Compiler -PathType Leaf)) {
    throw "C++ compiler not found: $Compiler. Install MinGW-w64 or pass -Compiler with the full path to g++.exe."
}

$sources = @('entry.cpp', 'backend.cpp', 'menu_ui.cpp', 'reference_catalog.cpp', 'native_dev_catalog.cpp', 'overlay_renderer.cpp', 'screen_capture.cpp', 'camera_debug_runtime.cpp', 'controller_input.cpp', 'game_input_capture.cpp', 'photo_mode_gate.cpp', 'engine_dispatch.cpp', 'debug_settings.cpp', 'reload_runtime.cpp', 'primary_objective_runtime.cpp', 'actor_save_runtime.cpp', 'replay_runtime.cpp', 'reload_options_runtime.cpp', 'reload_selection_runtime.cpp', 'reload_mission_catalog.cpp') | ForEach-Object {
    $path = Join-Path (Join-Path $PSScriptRoot 'src') $_
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Source file missing: $path" }
    $path
}
$imgui = Join-Path $PSScriptRoot 'vendor\imgui'
foreach ($name in @('imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp','backends\imgui_impl_dx12.cpp','backends\imgui_impl_win32.cpp')) {
    $path = Join-Path $imgui $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Pinned ImGui source missing: $path" }
    $sources += $path
}
$metadata = Join-Path $PSScriptRoot 'info.json'
if (-not (Test-Path -LiteralPath $metadata -PathType Leaf)) { throw "Package metadata missing: $metadata" }
$null = Get-Content -LiteralPath $metadata -Raw | ConvertFrom-Json

$dist = Join-Path $PSScriptRoot 'dist'
$null = New-Item -ItemType Directory -Path $dist -Force
$buildId = [Guid]::NewGuid().ToString('N')
$temporaryDll = Join-Path $dist ".sm2debugmenu.$buildId.dll"
$temporaryPreview = Join-Path $dist ".SM2DebugMenuPreview.$buildId.exe"
$temporaryPackage = Join-Path $dist ".SM2DebugMenu.$buildId.script"
$dll = Join-Path $dist 'sm2debugmenu.dll'
$preview = Join-Path $dist 'SM2DebugMenuPreview.exe'
$package = Join-Path $dist 'SM2DebugMenu.script'
$common = @('-std=c++17', '-O2', '-Wall', '-Wextra', '-DUNICODE', '-D_UNICODE', '-DNOMINMAX',
    '-static', '-static-libgcc', '-static-libstdc++', '-I', $imgui, '-I', (Join-Path $imgui 'backends'))
$libraries = @('-luser32', '-lgdi32', '-lcomctl32', '-lpsapi', '-lbcrypt', '-ld3dcompiler', '-ldwmapi', '-lole32', '-luuid', '-lwindowscodecs', '-lhid', '-lsetupapi')

$originalCompilerPath = $env:Path
try {
    $env:Path = (Split-Path -Parent $Compiler) + ';' + $env:Path
    Write-Host 'Building the x64 debug-menu DLL...'
    $arguments = $common + @('-shared', '-o', $temporaryDll) + $sources + $libraries
    & $Compiler @arguments
    if ($LASTEXITCODE -ne 0) { throw "DLL compilation failed (g++ exit code $LASTEXITCODE)." }
    if (-not (Test-Path -LiteralPath $temporaryDll -PathType Leaf)) { throw 'Compiler produced no DLL.' }

    if (-not $SkipPreview) {
        Write-Host 'Building the standalone preview...'
        $arguments = $common + @('-DSM2DEBUG_PREVIEW', '-municode', '-mwindows', '-o', $temporaryPreview) + $sources + $libraries
        & $Compiler @arguments
        if ($LASTEXITCODE -ne 0) { throw "Preview compilation failed (g++ exit code $LASTEXITCODE)." }
        if (-not (Test-Path -LiteralPath $temporaryPreview -PathType Leaf)) { throw 'Compiler produced no preview executable.' }
    }

    Add-Type -AssemblyName System.IO.Compression
    $stream = [IO.File]::Open($temporaryPackage, [IO.FileMode]::CreateNew)
    try {
        $archive = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Create, $true)
        try {
            foreach ($file in @(@{ Source = $metadata; Name = 'info.json' }, @{ Source = $temporaryDll; Name = 'sm2debugmenu.dll' })) {
                $entry = $archive.CreateEntry($file.Name, [IO.Compression.CompressionLevel]::Optimal)
                $output = $entry.Open()
                try {
                    $input = [IO.File]::OpenRead($file.Source)
                    try { $input.CopyTo($output) } finally { $input.Dispose() }
                } finally { $output.Dispose() }
            }
        } finally { $archive.Dispose() }
    } finally { $stream.Dispose() }

    Move-Item -LiteralPath $temporaryDll -Destination $dll -Force
    if (-not $SkipPreview) { Move-Item -LiteralPath $temporaryPreview -Destination $preview -Force }
    Move-Item -LiteralPath $temporaryPackage -Destination $package -Force
    Write-Host "Built: $dll"
    if (-not $SkipPreview) { Write-Host "Preview: $preview" }
    Write-Host "Overstrike package: $package"
    Get-FileHash -LiteralPath $dll, $package -Algorithm SHA256 | Format-Table Path, Hash -AutoSize
} finally {
    $env:Path = $originalCompilerPath
    foreach ($path in @($temporaryDll, $temporaryPreview, $temporaryPackage)) {
        if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
    }
}
