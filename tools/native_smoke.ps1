param(
    [string]$Runner = "$PSScriptRoot/../build/native/native/recompiler/Release/hmawl_original_startup.exe",
    [string]$Dol = "$PSScriptRoot/../build/game-us-r0/main.dol",
    [string]$Rvz = "$PSScriptRoot/../Harvest Moon - A Wonderful Life (USA).rvz",
    [ValidateRange(1, 2147483647)][int]$MaxBlocks = 50000000,
    [ValidateRange(0, 65536)][int]$TraceBlocks = 128,
    [ValidateRange(0, 65536)][int]$TraceInstructions = 0,
    [string]$DiagnosticsDirectory = "$PSScriptRoot/../diagnostics"
)
$ErrorActionPreference = 'Stop'
$runDirectory = Join-Path $DiagnosticsDirectory ("smoke-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null
$log = Join-Path $runDirectory 'runner.log'
# Windows PowerShell treats a native stderr line as an ErrorRecord. Preserve it
# without converting an expected dependency/budget stop into a script exception.
$ErrorActionPreference = 'Continue'
& $Runner --original-startup $Dol --rvz $Rvz --max-blocks $MaxBlocks --display off --trace-blocks $TraceBlocks --trace-instructions $TraceInstructions --diagnostics $runDirectory *> $log
$runnerExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
$reports = @(Get-ChildItem -LiteralPath $runDirectory -Filter report.json -Recurse)
if ($reports.Count -ne 1) {
    throw "Runner exited $runnerExit without exactly one report. Inspect $log"
}
$report = Get-Content -LiteralPath $reports[0].FullName -Raw | ConvertFrom-Json
if ($report.exit_code -ne $runnerExit) { throw 'Reported exit code differs from process status.' }
if ($TraceInstructions -gt 0 -and
    (-not $report.instruction_trace_enabled -or @($report.recent_instructions).Count -eq 0)) {
    throw 'Instruction tracing was requested but no instruction entries were reported.'
}
[pscustomobject]@{
    exit_code = $runnerExit
    classification = $report.classification
    pc = $report.cpu.pc
    reason = $report.reason
    loops = $report.game_loop_iterations
    decoder = $report.decoder
    observed_instruction_entries = $report.observed_instruction_entries
    recent_instructions = $report.recent_instructions
    report = $reports[0].FullName
    log = $log
    runner_sha256 = (Get-FileHash -LiteralPath $Runner -Algorithm SHA256).Hash
} | ConvertTo-Json -Depth 5
exit $runnerExit
