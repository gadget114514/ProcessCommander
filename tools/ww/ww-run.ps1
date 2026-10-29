# ww-run.ps1 — run one task card (ww-data\tasks\<id>-*.md) with agy, opencode, claude or codex, safely and visibly.
# The checkout root is detected (see Resolve-WwPaths); pass -Root when the current directory is
# not inside the project.
# Usage:  Tools\ww\ww-run.cmd 012                     (main checkout)
#         Tools\ww\ww-run.cmd 012 -Root C:\path\proj  (run against another checkout)
#         Tools\ww\ww-run.cmd 012 -Engine opencode    (agy is the default; also claude, codex)
#         Tools\ww\ww-run.cmd 012 -Engine claude -Model sonnet
#         Tools\ww\ww-run.cmd 012 -Worktree            (parallel run in .claude\worktrees\ww-012)
#         Tools\ww\ww-run.cmd 012 -Worktree -Base main
#         Tools\ww\ww-run.cmd 012 -Worktree -DryRun    (print the plan, change nothing)
param(
    [Parameter(Mandatory = $true)][string]$Task,
    [string]$Extra = "",
    [switch]$Worktree,
    [string]$Base = "main",
    [ValidateSet("agy", "opencode", "claude", "codex")][string]$Engine = $(if ($env:WW_ENGINE) { $env:WW_ENGINE } else { "agy" }),
    [string]$Model = $env:WW_MODEL,
    [switch]$JsonStdout = $(if ($env:WW_JSON_STDOUT -eq "1") { $true } else { $false }),
    [string]$Root = $env:WW_ROOT,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "ww-run.lib.ps1")
try { $paths = Resolve-WwPaths -Root $Root }
catch { Write-Host "[ww-run] $($_.Exception.Message)" -ForegroundColor Red; exit 1 }
$root = $paths.Root
$data = $paths.DataDir
$logDir = Join-Path $data "logs"
$runDir = Join-Path $data "running"
$reportsDir = Join-Path $data "reports"
$runningFile = Join-Path $data "running.json"
$lastFile = Join-Path $data "last_run.json"
$taskRunFile = Join-Path $runDir "$Task.json"
$taskLastFile = Join-Path $runDir "$Task.last.json"
Set-Location $root

# 1. Resolve the task card. -DryRun still prints a plan when the card is missing.
$card = Get-ChildItem (Join-Path $data "tasks") -Filter "$Task-*.md" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $card -and -not $DryRun) {
    Write-Host "[ww-run] task card $Task-*.md not found" -ForegroundColor Red
    exit 1
}
if (-not $card) {
    Write-Host "[ww-run] DRYRUN warning: task card $Task-*.md not found (a real run would exit 1)" -ForegroundColor Yellow
}
$cardPath = if ($card) { $card.FullName -replace '\\', '/' }
            else { (Join-Path (Join-Path $data "tasks") "$Task-*.md") -replace '\\', '/' }

# 2. Plan the worktree (no side effects yet).
$plan = $null
if ($Worktree) {
    $plan = Get-WorktreePlan -Root $root -Task $Task -Base $Base
    Write-Host ("[ww-run] {0} worktree {1}  branch {2}  base {3}" -f
        $(if ($DryRun) { "DRYRUN" } else { "planned" }), $plan.Path, $plan.Branch, $plan.Base) -ForegroundColor Cyan
}

# 3. Pre-flight: per-task duplicate check and (for -Worktree) tracked-files check.
$runs = @(Get-WorkerRuns)
# Remove stale per-task records before the duplicate check (a dead run must not block a new one).
# -DryRun stays free of side effects, so it neither cleans nor creates anything.
if (-not $DryRun) { $null = Clear-DeadRunRecords -DataDir $data -ScriptName "ww-run" -Runs $runs }
$conflict = Test-RunConflict -Runs $runs -Task $Task -Worktree:$Worktree
if ($conflict.Blocked) {
    Write-Host "[ww-run] not starting: $($conflict.Reason)." -ForegroundColor Red
    $conflict.Runs | ForEach-Object { Write-Host ("  PID {0}: {1}" -f $_.ProcessId, $_.CommandLine) }
    Write-Host "  Check progress: Tools\ww\ww-status.cmd   Stop it: Stop-Process -Id <PID>"
    if (-not $DryRun) { exit 1 }
    Write-Host "[ww-run] DRYRUN: a real run would refuse to start." -ForegroundColor Yellow
}

$untracked = @()
if ($Worktree -and $card) {
    $untracked = @(Get-UntrackedAuthorisedPaths -CardPath $card.FullName -RepoRoot $root)
    if ($untracked.Count -gt 0) {
        foreach ($p in $untracked) { Write-Host "[ww-run] worktree pre-flight: $p is not tracked in git" -ForegroundColor Red }
        Write-Host "[ww-run] needs tracked files; run on main or land 150 first" -ForegroundColor Red
        if (-not $DryRun) { exit 1 }
    }
}

# 4. Create the worktree, or reuse it when it already exists on the expected branch.
if ($Worktree -and -not $DryRun) {
    if (Test-Path $plan.Path) {
        $current = (& git -C $plan.Path branch --show-current 2>$null | Select-Object -First 1)
        if ($current -ne $plan.Branch) {
            Write-Host "[ww-run] worktree $($plan.Path) is on branch '$current', expected '$($plan.Branch)'; not touching it." -ForegroundColor Red
            exit 1
        }
        Write-Host "[ww-run] reusing worktree $($plan.Path) on $($plan.Branch)"
    } else {
        New-Item -ItemType Directory -Force (Split-Path -Parent $plan.Path) | Out-Null
        & git -C $root rev-parse --verify --quiet "refs/heads/$($plan.Branch)" | Out-Null
        if ($LASTEXITCODE -eq 0) {
            & git -C $root worktree add $plan.Path $plan.Branch
        } else {
            & git -C $root worktree add $plan.Path -b $plan.Branch $plan.Base
        }
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[ww-run] git worktree add failed (exit $LASTEXITCODE)" -ForegroundColor Red
            exit 1
        }
    }
}

# Worktree bootstrap (card 190): opencode rejects reading files outside its working directory,
# so the card is copied into the worktree and referenced by its relative path. The gitignored
# opencode.json is copied too when missing (never overwritten). The report is written inside the
# worktree and copied back to the main ww-data/reports after the run.
$promptCard = $cardPath
if ($Worktree) {
    if (-not $DryRun) {
        $wtTasks = Join-Path (Join-Path $plan.Path $paths.RelData) "tasks"
        New-Item -ItemType Directory -Force $wtTasks | Out-Null
        if ($card) { Copy-Item -LiteralPath $card.FullName -Destination (Join-Path $wtTasks $card.Name) -Force }
        if ($Engine -eq 'opencode') {
            $wtConfig = Join-Path $plan.Path "opencode.json"
            if (-not (Test-Path -LiteralPath $wtConfig)) {
                $mainConfig = Join-Path $root "opencode.json"
                if (Test-Path -LiteralPath $mainConfig) { Copy-Item -LiteralPath $mainConfig -Destination $wtConfig -Force }
            }
        }
    }
    if ($card) { $promptCard = "$($paths.RelUrl)/tasks/$($card.Name)" }
}
$prompt = "Execute $promptCard. Read AGENTS.md first. $Extra".Trim()
$workDir = $root
if ($Worktree) {
    $workDir = $plan.Path
    $prompt += " WORKTREE RUN: work only inside the worktree $($plan.Path). Skip compile/tests; list them under 'Verification pending' in the report. Write the report to $($paths.RelUrl)/reports/$Task.md inside the worktree and commit it with git add -f; do not merge to main. The copied task card is for reading only; do not commit it."
}

if ($DryRun) {
    $cardDisplay = $promptCard
    if ($Worktree -and $card) { $cardDisplay = "$promptCard (copied into the worktree from $cardPath)" }
    Write-Host "[ww-run] DRYRUN plan:"
    Write-Host "  engine      : $Engine"
    Write-Host "  model       : $(if ($Model) { $Model } elseif ($Engine -eq 'agy') { 'gemini-3.8-flash-medium (default)' } else { '(engine default)' })"
    Write-Host "  command     : $(Get-EngineCommand -Engine $Engine -Model $Model)"
    Write-Host "  data dir    : $data"
    Write-Host "  mode        : $(if ($Worktree) { 'worktree' } else { 'main' })"
    Write-Host "  card        : $cardDisplay"
    Write-Host "  working dir : $workDir"
    Write-Host "  branch      : $(if ($Worktree) { "$($plan.Branch) from $($plan.Base)" } else { 'n/a (main checkout)' })"
    Write-Host "  run record  : $taskRunFile"
    Write-Host "  last record : $taskLastFile$(if ($Worktree) { ' (main last_run.json is not touched)' } else { ", $lastFile" })"
    Write-Host "  log         : $logDir\$Task-<stamp>.log"
    Write-Host "  duplicate   : $(if ($conflict.Blocked) { 'WOULD REFUSE' } else { 'ok' })"
    Write-Host "  pre-flight  : $(if ($untracked.Count) { 'WOULD REFUSE: ' + ($untracked -join ', ') } else { 'ok' })"
    if ($conflict.Blocked -or $untracked.Count) { exit 1 } else { exit 0 }
}

# 5. Record what is running (one record per task; running.json/last_run.json stay main-only).
New-Item -ItemType Directory -Force $logDir, $runDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$log = Join-Path $logDir "$Task-$stamp.log"
$progress = Join-Path $logDir "$Task-$stamp.progress.log"
$events = Join-Path $logDir "$Task-$stamp.events.jsonl"
$promptFile = Join-Path $logDir "$Task-$stamp.prompt.txt"
$start = Get-Date
$record = @{
    Task        = $Task
    Card        = $card.Name
    Log         = $log
    Progress    = $progress
    Events      = $events
    Started     = $start.ToString("s")
    Host        = $PID
    Worktree    = $(if ($Worktree) { $plan.Path } else { $null })
    Engine      = $Engine
    WorkerPid   = $null
    OpencodePid = $null
}
$record | ConvertTo-Json | Set-Content -Encoding UTF8 $taskRunFile
if (-not $Worktree) { $record | ConvertTo-Json | Set-Content -Encoding UTF8 $runningFile }

Write-Host "[ww-run] task $Task ($($card.Name)) started $($start.ToString('HH:mm:ss'))" -ForegroundColor Cyan
if ($Worktree) { Write-Host "[ww-run] worktree: $($plan.Path)  branch: $($plan.Branch)" -ForegroundColor Cyan }
Write-Host "[ww-run] log: $log"

# 6. Run the engine with stdin closed (step 0: otherwise it waits on stdin and prints nothing) and
# read the stream-json/json event stream line by line as it arrives. Raw events go to the .jsonl
# file; Format-ProgressEvent writes one readable line to the .progress.log and the console.
$exit = 0
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($events, "", $utf8)
[System.IO.File]::WriteAllText($progress, ("{0}  start  task {1}" -f $start.ToString("HH:mm:ss"), $Task) + "`r`n", $utf8)
Push-Location $workDir
try {
    "started $($start.ToString('s')) prompt: $prompt" | Set-Content -Encoding UTF8 $log
    [System.IO.File]::WriteAllText($promptFile, $prompt, $utf8)

    # The prompt rides in a UTF-8 file so quotes/backslashes in it cannot break the command line.
    # -EncodedCommand avoids cmd.exe quoting entirely; the child closes stdin immediately (below).
    # stderr is left on the console (raw); JSON errors on stdout still reach the progress log.
    $safePrompt = $promptFile.Replace("'", "''")
    $engineCommand = Get-EngineCommand -Engine $Engine -Model $Model
    $launch = "`$ProgressPreference='SilentlyContinue'; `$prompt = (Get-Content -LiteralPath '$safePrompt' -Raw -Encoding UTF8).TrimEnd(); $engineCommand; if (`$null -eq `$LASTEXITCODE) { exit 1 } else { exit `$LASTEXITCODE }"
    $encoded = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($launch))

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "powershell.exe"
    $psi.Arguments = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $encoded"
    $psi.WorkingDirectory = $workDir
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $psi.CreateNoWindow = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    $proc.StandardInput.Close()

    $workerPid = Find-WorkerPid -Task $Task
    if ($workerPid) {
        $record.WorkerPid = $workerPid
        $record.OpencodePid = $workerPid
        $record | ConvertTo-Json | Set-Content -Encoding UTF8 $taskRunFile
    }

    while ($null -ne ($line = $proc.StandardOutput.ReadLine())) {
        [System.IO.File]::AppendAllText($events, $line + "`r`n", $utf8)
        if ($JsonStdout) {
            [Console]::Out.WriteLine($line)
        }
        $progressLine = Format-ProgressEvent -Line $line
        if ($progressLine) {
            [System.IO.File]::AppendAllText($progress, $progressLine + "`r`n", $utf8)
            if (-not $JsonStdout) {
                Write-Host $progressLine
            }
        }
    }
    $proc.WaitForExit()
    $exit = $proc.ExitCode
} catch {
    $exit = 1
    $_ | Out-String | Add-Content $log
} finally {
    Pop-Location
    # The agent wrote the report inside the worktree; bring it back for the main checkout.
    if ($Worktree) {
        $wtReport = Join-Path (Join-Path $plan.Path $paths.RelData) "reports\$Task.md"
        if (Test-Path -LiteralPath $wtReport) {
            New-Item -ItemType Directory -Force $reportsDir | Out-Null
            Copy-Item -LiteralPath $wtReport -Destination (Join-Path $reportsDir "$Task.md") -Force
        }
    }
    $end = Get-Date
    $mins = [math]::Round(($end - $start).TotalMinutes, 1)
    $reports = @(Get-ReportsWritten -ReportsDir $reportsDir -Started $start -Task $Task)
    $last = @{
        Task           = $Task
        Card           = $card.Name
        Log            = $log
        Started        = $start.ToString("s")
        Ended          = $end.ToString("s")
        Minutes        = $mins
        ExitCode       = $exit
        ReportsWritten = @($reports)
        Worktree       = $(if ($Worktree) { $plan.Path } else { $null })
        Engine         = $Engine
    }
    $last | ConvertTo-Json | Set-Content -Encoding UTF8 $taskLastFile
    if (-not $Worktree) { $last | ConvertTo-Json | Set-Content -Encoding UTF8 $lastFile }
    Remove-Item $taskRunFile -ErrorAction SilentlyContinue
    if (-not $Worktree) { Remove-Item $runningFile -ErrorAction SilentlyContinue }

    $msg = if ($reports) { "Task $Task finished in $mins min. Report: $($reports -join ', ')" }
           else { "Task $Task ended in $mins min WITHOUT a phase report (exit $exit). See log." }
    $color = if ($reports -and $exit -eq 0) { "Green" } else { "Yellow" }
    Write-Host "[ww-run] $msg" -ForegroundColor $color
    [console]::beep(880, 300)
    try { (New-Object -ComObject WScript.Shell).Popup($msg, 15, "ww-run", 64) | Out-Null } catch {}
}
exit $exit
