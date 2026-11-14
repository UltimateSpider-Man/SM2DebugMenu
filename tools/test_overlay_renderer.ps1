[CmdletBinding()]
param([string]$PreviewExecutable = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Split-Path -Parent $PSScriptRoot
$game = Split-Path -Parent $project
$preview = if ($PreviewExecutable) { [IO.Path]::GetFullPath($PreviewExecutable) } else { Join-Path $project 'dist\SM2DebugMenuPreview.exe' }
$previewDirectory = Split-Path -Parent $preview
$previewOutput = if ((Split-Path -Leaf $previewDirectory) -eq 'dist') { $project } else { $previewDirectory }
$expectedBackend = if (Test-Path -LiteralPath (Join-Path $game 'dxgi.dll')) { 'private' } else { 'native' }
$verification = Join-Path $project 'verification'
$null = New-Item -ItemType Directory -Path $verification -Force
if (@(Get-Process -Name Spider-Man2 -ErrorAction SilentlyContinue).Count) {
    throw 'The game is running. Preserve that session and run this isolated GPU preview after it closes.'
}
if (-not (Test-Path -LiteralPath $preview -PathType Leaf)) { throw 'Build the actual overlay preview before running this check.' }
$priorConfig = $env:DXVK_CONFIG_FILE
$priorDxvkLog = $env:DXVK_LOG_PATH
$priorVkd3dLog = $env:VKD3D_LOG_FILE
$priorCache = $env:VKD3D_SHADER_CACHE_PATH
$cases = @()
try {
    $env:DXVK_CONFIG_FILE = Join-Path $game 'LTSC2019Compatibility\dxvk.conf'
    $env:DXVK_LOG_PATH = $verification
    $env:VKD3D_SHADER_CACHE_PATH = Join-Path $verification 'build'
    foreach ($case in @(
        @{ Name = 'baseline'; Argument = '--overlay-smoke-test'; Fault = $false },
        @{ Name = 'partial-resource'; Argument = '--overlay-fault-resize-test'; Fault = $true }
    )) {
        $eventDirectory = Join-Path $previewOutput 'verification'
        $null = [IO.Directory]::CreateDirectory($eventDirectory)
        $eventPath = Join-Path $eventDirectory 'overlay-ui-events.jsonl'
        $priorEventCount = if (Test-Path -LiteralPath $eventPath) { @(Get-Content -LiteralPath $eventPath).Count } else { 0 }
        $env:VKD3D_LOG_FILE = Join-Path $verification ("overlay-$($case.Name)-vkd3d.log")
        $start = New-Object Diagnostics.ProcessStartInfo
        $start.FileName = $preview
        $start.Arguments = $case.Argument
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $process = New-Object Diagnostics.Process
        $process.StartInfo = $start
        $null = $process.Start()
        Write-Host "Owned actual GPU preview ($($case.Name)) PID $($process.Id)"
        if (-not $process.WaitForExit(45000)) {
            $process.Kill()
            throw "Owned preview $($case.Name) exceeded 45 seconds and was stopped."
        }
        if ($process.ExitCode -ne 0) { throw "Actual GPU preview $($case.Name) failed with exit code $($process.ExitCode). See overlay.log." }
        $reportPath = Join-Path $previewOutput 'overlay-preview-test.json'
        $result = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
        if ($result.PSObject.Properties.Name -notcontains 'capture_source' -or
            $result.capture_source -cne 'actual_D3D12_backbuffer_readback_before_Present') {
            throw 'Rebuild the preview with actual D3D12 readback capture; desktop/window pixel captures are not sufficient evidence.'
        }
        if (-not $result.passed -or -not $result.actual_d3d12_device -or -not $result.actual_swapchain -or
            $result.graphics_backend -cne $expectedBackend -or
            ($expectedBackend -ceq 'native' -and (-not $result.actual_native_swapchain -or $result.actual_private_swapchain)) -or
            ($expectedBackend -ceq 'private' -and (-not $result.actual_private_swapchain -or $result.actual_native_swapchain)) -or
            -not $result.hidden_frames_submit_no_gpu_work -or $result.rendered_overlay_frames -lt 30 -or
            $result.resizes -lt 2 -or $result.game_started -or $result.gameplay_bindings_enabled) {
            throw "Actual GPU preview $($case.Name) did not establish rendering, resize and hidden-frame requirements."
        }
        if (-not $result.debug_root_gpu_capture -or -not $result.dev_root_gpu_capture -or -not $result.dev_root_navigation_queued -or
            $result.debug_vertices -le 500 -or $result.dev_vertices -le 500 -or $result.debug_white_pixels -le 100 -or $result.dev_white_pixels -le 100) {
            throw 'Both reference styles must contain actual rendered geometry and visible text in separate GPU captures.'
        }
        if (-not $result.production_display_capture_verified -or -not $result.capture_resize_released_backbuffer) {
            throw 'Production display capture must save exact pre-overlay pixels and release its source for resize.'
        }
        $events = @(Get-Content -LiteralPath $eventPath | Select-Object -Skip $priorEventCount | ForEach-Object { $_ | ConvertFrom-Json })
        if (-not @($events | Where-Object { $_.node -ceq 'debug' -and $_.input -eq 0 }).Count -or
            -not @($events | Where-Object { $_.node -ceq 'dev' -and $_.input -eq 8 }).Count) {
            throw 'Fresh production UI events did not prove Toggle on Debug and shoulder-button navigation to Dev.'
        }
        if ($case.Fault -and (-not $result.partial_resource_fault_injected -or -not $result.partial_resource_resize_recovered)) {
            throw 'Injected partial-resource case did not demonstrate a successful real swapchain resize and recovery.'
        }
        $outputReport = Join-Path $verification ("overlay-$($case.Name)-test.json")
        $outputImage = Join-Path $verification ("overlay-$($case.Name).bmp")
        Copy-Item -LiteralPath $reportPath -Destination $outputReport -Force
        Copy-Item -LiteralPath (Join-Path $previewOutput 'overlay-preview.bmp') -Destination $outputImage -Force
        $styleImages = [ordered]@{}
        foreach ($style in @('debug', 'dev')) {
            $styleImage = Join-Path $verification ("overlay-$($case.Name)-$style.bmp")
            Copy-Item -LiteralPath (Join-Path $previewOutput "overlay-$style.bmp") -Destination $styleImage -Force
            $styleImages[$style] = [ordered]@{ path = $styleImage; sha256 = (Get-FileHash -LiteralPath $styleImage -Algorithm SHA256).Hash.ToLowerInvariant() }
        }
        if ($styleImages.debug.sha256 -ceq $styleImages.dev.sha256) { throw 'Debug and Dev GPU images must demonstrate different layouts.' }
        $cases += [ordered]@{
            name = $case.Name
            command = @($preview, $case.Argument)
            owned_process_id = $process.Id
            exit_code = $process.ExitCode
            result = $result
            screenshot = $outputImage
            screenshot_sha256 = (Get-FileHash -LiteralPath $outputImage -Algorithm SHA256).Hash.ToLowerInvariant()
            styles = $styleImages
            fresh_ui_events = $events
        }
    }
    $report = [ordered]@{
        schema_version = 1
        tested_at_utc = [DateTime]::UtcNow.ToString('o')
        preview_sha256 = (Get-FileHash -LiteralPath $preview -Algorithm SHA256).Hash.ToLowerInvariant()
        renderer_source_sha256 = (Get-FileHash -LiteralPath (Join-Path $project 'src\overlay_renderer.cpp') -Algorithm SHA256).Hash.ToLowerInvariant()
        ui_source_sha256 = (Get-FileHash -LiteralPath (Join-Path $project 'src\menu_ui.cpp') -Algorithm SHA256).Hash.ToLowerInvariant()
        catalog_source_sha256 = (Get-FileHash -LiteralPath (Join-Path $project 'src\reference_catalog.cpp') -Algorithm SHA256).Hash.ToLowerInvariant()
        plugin_sha256 = (Get-FileHash -LiteralPath (Join-Path $project 'dist\sm2debugmenu.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
        actual_gpu_cases = $cases
        game_started = $false
        limitation = 'These are genuine GPU/swapchain preview tests, including a failed-resource resize regression. Live game rendering, physical controller input, and gameplay crash stability require separate evidence.'
    }
    [IO.File]::WriteAllText((Join-Path $verification 'overlay-renderer-tests.json'), ($report | ConvertTo-Json -Depth 10), [Text.Encoding]::UTF8)
    $cases | ForEach-Object { [pscustomobject]@{ name = $_.name; exit_code = $_.exit_code; passed = $_.result.passed; overlay_frames = $_.result.rendered_overlay_frames; resizes = $_.result.resizes } } | Format-Table -AutoSize
} finally {
    $env:DXVK_CONFIG_FILE = $priorConfig
    $env:DXVK_LOG_PATH = $priorDxvkLog
    $env:VKD3D_LOG_FILE = $priorVkd3dLog
    $env:VKD3D_SHADER_CACHE_PATH = $priorCache
}
