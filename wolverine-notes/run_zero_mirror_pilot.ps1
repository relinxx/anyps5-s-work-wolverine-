param(
    [ValidateRange(0, 300)][int]$DelaySeconds = 45,
    [ValidateRange(1, 900)][int]$RunSeconds = 900,
    [switch]$CheckOnly,
    [switch]$Control,
    [switch]$Only10d,
    [switch]$GpuHeapCompare
)

$ErrorActionPreference = 'Stop'
if ($Control -and $Only10d) { throw 'Choose control or 10d-only mode, not both.' }
$workspace = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$pythonRuntime = 'C:\Users\pc\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
$launcher = Join-Path $PSScriptRoot 'launch_michon.py'
$runRoot = Join-Path $workspace 'work\wolverine-michon-runs'
$experimentName = if ($Control) { 'zero-mirror-control' } elseif ($Only10d) { 'zero-mirror-10d' } else { 'zero-mirror-pilot' }
$resultLog = Join-Path $PSScriptRoot "last-$experimentName-launch.txt"
$runPointer = Join-Path $PSScriptRoot "latest-$experimentName-run.txt"

# Reproduce the diagnostic environment in 20261013T034429Z-e9a3f107, with only the
# explicit pilot flag added. The chosen duration is bounded to 900s; reserve guards remain active.
$launcherArguments = @(
    '--seconds', [string]$RunSeconds,
    '--host-import-mib', '8192', '--prefer-host-imports',
    '--host-import-window-mib', '128', '--trace-host-import', '--no-whole-range-import',
    '--device-heap-mirrors', '--device-heap-descriptor-mirrors', '--reuse-device-heap-mirrors',
    '--file-backed-direct-memory', '--trim-mirror-free-mib', '4096',
    '--trim-source-range', '--trim-refresh-source-range', '--trim-pressure-source-free-mib', '2048',
    '--stream-heap-writeback-mib', '64', '--heap-mirror-mib', '8192', '--trace-gpu-memory',
    '--heap-optimize-free-mib', '4096', '--memory-ledger', '--memory-ledger-interval-ms', '10000',
    '--import-zero-audit', '--trace-ir-reclaim', '--demand-registration-prepare',
    '--prepare-workers', '2', '--prepare-queue-mib', '64', '--prepare-queue-entries', '32',
    '--resource-cache-entries', '32', '--dispatch-cache-entries', '64',
    '--recorder-kept-mib', '128', '--staging-pool-mib', '64', '--host-buffer-pool-mib', '64',
    '--texture-cache-mib', '1024', '--unit-shadow-mib', '1024',
    '--frame-limit', '256', '--dump-every', '64', '--allocation-audit', '--zero-mirror-pilots'
)
if ($Control) {
    $launcherArguments = @($launcherArguments | Where-Object { $_ -ne '--zero-mirror-pilots' })
} elseif ($Only10d) {
    $launcherArguments = @($launcherArguments | Where-Object { $_ -ne '--zero-mirror-pilots' }) + @('--zero-mirror-10d-only', '--preserve-11e-imports')
}
if ($GpuHeapCompare) { $launcherArguments += '--gpu-heap-compare' }

if ($CheckOnly) {
    & $pythonRuntime $launcher @launcherArguments --check-only
    if ($LASTEXITCODE -ne 0) { throw 'Pilot settings/runtime validation failed.' }
    return
}

Write-Host "$experimentName launch in $DelaySeconds seconds. Close other applications; leave this PowerShell window open."
Start-Sleep -Seconds $DelaySeconds
$game = Get-Process wolverine -ErrorAction SilentlyContinue
if ($game) { throw 'A game instance is already running. Close it before this test.' }
$diskGiB = (Get-PSDrive C).Free / 1GB
$freeRamGiB = (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB
if ($diskGiB -lt 30) { throw "Only $([Math]::Round($diskGiB,2)) GiB free on C:. Free at least 30 GiB before this test." }
$baselineReport = Join-Path $runRoot '20261013T045031Z-3e7fe10d\long-pilot-report.json'
$baseline = (Get-Content -LiteralPath $baselineReport -Raw | ConvertFrom-Json).initial_free_gib
if ($freeRamGiB -lt ($baseline - 0.5)) { throw "Starting free RAM $([Math]::Round($freeRamGiB,2)) GiB is below the matched baseline ($([Math]::Round($baseline,2)) GiB minus 0.5 GiB). Close other apps before retrying." }
Write-Host "Preflight: C free $([Math]::Round($diskGiB,2)) GiB; RAM available $([Math]::Round($freeRamGiB,2)) GiB; baseline $([Math]::Round($baseline,2)) GiB."
$existingRuns = @(Get-ChildItem -LiteralPath $runRoot -Directory | ForEach-Object { $_.Name })
try {
    & $pythonRuntime $launcher @launcherArguments | Tee-Object -FilePath $resultLog
    $launcherExit = $LASTEXITCODE
} finally {
    $createdRun = Get-ChildItem -LiteralPath $runRoot -Directory |
        Where-Object { $_.Name -notin $existingRuns } |
        Sort-Object CreationTime -Descending |
        Select-Object -First 1
    if ($null -ne $createdRun) {
        $createdRun.FullName | Set-Content -LiteralPath $runPointer -Encoding UTF8
        Write-Host "REPORT THIS RUN FOLDER: $($createdRun.FullName)"
        Write-Host "Its path is also saved in: $runPointer"
        if (Test-Path -LiteralPath (Join-Path $createdRun.FullName 'run.json')) {
            & $pythonRuntime (Join-Path $PSScriptRoot 'summarize_long_zero_pilot.py') $createdRun.FullName
            if ($LASTEXITCODE -ne 0) { Write-Warning 'Automatic report failed; raw run logs remain available.' }
        }
    } else {
        Write-Host "No new run folder was created. Report the launcher error instead."
    }
}
if ($launcherExit -ne 0) {
    $finished = if ($createdRun) { Get-Content -LiteralPath (Join-Path $createdRun.FullName 'run.json') -Raw | ConvertFrom-Json } else { $null }
    $expectedTimer = $finished -and $finished.capture_complete -and @($finished.limit_stop).Count -gt 0 -and
        @($finished.limit_stop | Where-Object { $_.reason -ne 'configured duration reached' }).Count -eq 0
    if (!$expectedTimer) { throw "Pilot launcher exited with code $launcherExit. See $resultLog" }
    Write-Host 'The requested duration completed; the collector stopped the game as configured.'
}
