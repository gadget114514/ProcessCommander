# ww-run.lib.ps1 — pure helpers for ww-run.ps1: worktree naming, the per-task duplicate rule
# and the tracked-files pre-flight. Dot-source this file; it defines functions only and has no
# side effects, so ww-run.selftest.ps1 can exercise the decisions without starting a run.

# Checkout root and task-data folder; nothing project specific is hardcoded.
#   root : -Root / WW_ROOT, else the git top level of the current directory, else the current directory.
#   data : -DataDir / WW_DATA_DIR, else <root>\ww-data (tasks\, reports\, logs\, running\ ...).
# RelData is the data folder relative to root (normally ww-data); worktree runs use it to find
# the same folder inside the worktree.
# Watch comes from <data>\config.json, e.g. { "watch": ["src", "Tools"] }: the folders (relative
# to root) whose changes count as run progress. Empty means the whole checkout except the data folder.
function Resolve-WwPaths {
    param([string]$Root = $env:WW_ROOT, [string]$DataDir = $env:WW_DATA_DIR)
    if (-not $Root) {
        $previous = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $top = & git rev-parse --show-toplevel 2>$null | Select-Object -First 1
        $ok = ($LASTEXITCODE -eq 0 -and $top)
        $ErrorActionPreference = $previous
        $Root = if ($ok) { $top -replace '/', '\' } else { (Get-Location).Path }
    }
    $Root = $Root.TrimEnd('\', '/')
    if (-not $DataDir) { $DataDir = Join-Path $Root "ww-data" }
    $DataDir = $DataDir.TrimEnd('\', '/')
    $rel = if ($DataDir.StartsWith("$Root\", [StringComparison]::OrdinalIgnoreCase)) { $DataDir.Substring($Root.Length + 1) }
           else { "ww-data" }
    $watch = @()
    $configFile = Join-Path $DataDir "config.json"
    if (Test-Path -LiteralPath $configFile) {
        try { $config = Get-Content -LiteralPath $configFile -Raw | ConvertFrom-Json }
        catch { throw "cannot read ${configFile}: $($_.Exception.Message)" }
        $watch = @(@($config.watch) | Where-Object { $_ } | ForEach-Object { ([string]$_ -replace '\\', '/').Trim('/') })
    }
    return [pscustomobject]@{
        Root    = $Root
        DataDir = $DataDir
        RelData = $rel
        RelUrl  = $rel -replace '\\', '/'
        Watch   = $watch
    }
}

# Worker runs (agy or opencode)
# Engines: agy, opencode, claude (Claude Code), codex (Codex CLI).
# claude.exe / codex.exe are also what interactive sessions run as, so for those two only a
# command line that carries a ww prompt ("Execute ...tasks/<id>-...") counts as a worker run.
# Claude Code installed through npm runs as node.exe .../claude-code/cli.js.
function Test-WorkerProcess([object]$Proc) {
    if (-not $Proc) { return $false }
    $name = [string]$Proc.Name
    $cmd = [string]$Proc.CommandLine
    $wwPrompt = $cmd -match 'Execute\s+\S*tasks[\\/]'
    switch ($name) {
        'agy.exe'      { return $true }
        'opencode.exe' { return [bool]($cmd -match '\srun\s') }
        'claude.exe'   { return [bool]($wwPrompt -and $cmd -match '\s(-p|--print)\s') }
        'node.exe'     { return [bool]($wwPrompt -and $cmd -match 'claude-code[\\/]cli' -and $cmd -match '\s(-p|--print)\s') }
        'codex.exe'    { return [bool]($wwPrompt -and $cmd -match '\sexec\s') }
    }
    return $false
}

# Engine name for a worker process (ww-status display).
function Get-WorkerEngine([object]$Proc) {
    switch ([string]$Proc.Name) {
        'agy.exe'      { return 'agy' }
        'opencode.exe' { return 'opencode' }
        'codex.exe'    { return 'codex' }
    }
    return 'claude'
}

function Get-WorkerRuns {
    Get-CimInstance Win32_Process -Filter "Name='opencode.exe' or Name='agy.exe' or Name='claude.exe' or Name='codex.exe' or Name='node.exe'" -ErrorAction SilentlyContinue |
        Where-Object { Test-WorkerProcess $_ }
}
function Get-OpencodeRuns { Get-WorkerRuns }

# The command ww-run runs (inside a child powershell) for each engine. $Model is passed only when
# set; agy has a default model. The prompt is read from a UTF-8 file into $prompt by the caller.
function Get-EngineCommand {
    param([Parameter(Mandatory = $true)][string]$Engine, [string]$Model = "")
    $m = $Model.Replace("'", "''")
    switch ($Engine) {
        'opencode' { return "& opencode.cmd run --format json$(if ($Model) { " --model '$m'" }) `$prompt" }
        'agy'      { return "& agy.exe -p `$prompt --output-format stream-json --model '$(if ($Model) { $m } else { 'gemini-3.8-flash-medium' })' --dangerously-skip-permissions --print-timeout 180m" }
        'claude'   { return "& claude -p `$prompt --output-format stream-json --verbose --dangerously-skip-permissions$(if ($Model) { " --model '$m'" })" }
        'codex'    { return "& codex exec --json --dangerously-bypass-approvals-and-sandbox$(if ($Model) { " --model '$m'" }) `$prompt" }
    }
    throw "unknown engine '$Engine' (agy, opencode, claude, codex)"
}

# Worktree runs are recognised from the prompt/command line that ww-run puts into opencode or agy.
# Keeping this in one place makes "a worktree run never blocks the main run" testable.
function Test-WorktreeCommandLine([string]$CommandLine) {
    if (-not $CommandLine) { return $false }
    return [bool]($CommandLine -match 'WORKTREE RUN' -or $CommandLine -match '\.claude[\\/]worktrees[\\/]')
}

# Does a command line execute this exact card? The negative lookahead keeps task 901 apart from
# its child cards 901-1-*.md, and the backslash alternative covers the absolute Windows path
# that ww-run writes into the prompt.
function Test-CommandLineHasTask([string]$CommandLine, [string]$Task) {
    if (-not $CommandLine -or -not $Task) { return $false }
    $escaped = [regex]::Escape($Task)
    return [bool]($CommandLine -match "tasks[\\/]$escaped-(?!\d+-)")
}

# Plan for -Worktree: deterministic path/branch naming. No IO here (the caller creates it),
# which is what lets -DryRun print the plan without side effects.
function Get-WorktreePlan {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Task,
        [string]$Base = "main"
    )
    return [pscustomobject]@{
        Task   = $Task
        Path   = Join-Path (Join-Path $Root ".claude\worktrees") "ww-$Task"
        Branch = "ww/$Task-wt"
        Base   = $Base
    }
}

# Duplicate rule (card 170):
#   * the same task card never runs twice, in any mode;
#   * a second MAIN (non-worktree) run is refused, because two agents would edit the same checkout;
#   * a worktree run never blocks the main run and a main run never blocks a worktree run.
function Test-RunConflict {
    param(
        $Runs,
        [Parameter(Mandatory = $true)][string]$Task,
        [switch]$Worktree
    )
    $active = @(@($Runs) | Where-Object { $_ })
    $sameTask = @($active | Where-Object { Test-CommandLineHasTask -CommandLine $_.CommandLine -Task $Task })
    if ($sameTask.Count -gt 0) {
        return [pscustomobject]@{ Blocked = $true; Reason = "task $Task is already running"; Runs = $sameTask }
    }
    if (-not $Worktree) {
        $mainRuns = @($active | Where-Object { -not (Test-WorktreeCommandLine $_.CommandLine) })
        if ($mainRuns.Count -gt 0) {
            return [pscustomobject]@{ Blocked = $true; Reason = "another main-checkout run is active"; Runs = $mainRuns }
        }
    }
    return [pscustomobject]@{ Blocked = $false; Reason = ""; Runs = @() }
}

# Paths named (in backticks) in the card's "Authorised edits" section. Single .cs file names
# such as `MicroSplatChunk.cs` are kept too; the caller ignores entries that do not exist
# relative to the checkout, so only fully qualified paths are actually pre-flighted.
function Get-AuthorisedEditPaths {
    param([Parameter(Mandatory = $true)][string]$CardPath)
    $paths = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path -LiteralPath $CardPath)) { return @() }
    $inSection = $false
    foreach ($line in Get-Content -LiteralPath $CardPath) {
        if ($line -match '^##\s+Authori[sz]ed edits\s*$') { $inSection = $true; continue }
        if ($inSection -and $line -match '^##\s') { break }
        if (-not $inSection) { continue }
        foreach ($m in [regex]::Matches($line, '`([^`]+)`')) {
            $token = $m.Groups[1].Value.Trim()
            if (-not $token -or $token -match '\s' -or $token -match '^https?://') { continue }
            if ($token -notmatch '[\\/]' -and $token -notmatch '\.[A-Za-z0-9]+$') { continue }
            if (-not $paths.Contains($token)) { $paths.Add($token) }
        }
    }
    return @($paths)
}

# ---------- progress visibility (card 190) ----------
# The opencode --format json event stream is one JSON object per line. These helpers turn a line
# into one short readable progress line, and read the estimate / reports / liveness facts that
# ww-run and ww-status share. Everything here is pure or read-only so the self tests can drive it
# with sample events and fixture folders (no opencode).
function Shorten-Text([string]$Text, [int]$Max = 160) {
    if (-not $Text) { return "" }
    $s = ($Text -replace '\s+', ' ').Trim()
    if ($s.Length -gt $Max) { $s = $s.Substring(0, $Max) }
    return $s
}

# Estimate from the card line "Estimate 60 min (hard stop at 60 ...)" -> 60; $null when absent.
function Get-EstimateMinutes {
    param([Parameter(Mandatory = $true)][string]$CardPath)
    if (-not $CardPath -or -not (Test-Path -LiteralPath $CardPath)) { return $null }
    $text = Get-Content -LiteralPath $CardPath -Raw -ErrorAction SilentlyContinue
    if (-not $text) { return $null }
    $m = [regex]::Match([string]$text, 'Estimate\s+(\d+)\s*min', 'IgnoreCase')
    if ($m.Success) { return [int]$m.Groups[1].Value }
    return $null
}

# Short target for a tool call: file path for read/edit/write, command for bash/run_command, then title/input/parameters.
function Get-EventTarget([object]$Part) {
    if (-not $Part) { return "" }
    $tool = if ($Part.tool_name) { [string]$Part.tool_name }
            elseif ($Part.tool) { [string]$Part.tool }
            elseif ($Part.tool_info -and $Part.tool_info.name) { [string]$Part.tool_info.name }
            elseif ($Part.name) { [string]$Part.name }
            else { "" }
    $state = $Part.state
    $input = if ($Part.tool_info -and $Part.tool_info.parameters) { $Part.tool_info.parameters }
             elseif ($Part.parameters) { $Part.parameters }
             elseif ($state -and $state.input) { $state.input }
             elseif ($Part.input) { $Part.input }
             else { $null }
    if ($input) {
        if ($tool -in @('read', 'edit', 'write', 'multiedit', 'notebookedit', 'read_file', 'edit_file', 'write_file', 'write_to_file', 'view_file')) {
            $p = if ($input.filePath) { $input.filePath } elseif ($input.file_path) { $input.file_path } elseif ($input.notebook_path) { $input.notebook_path }
                 elseif ($input.Path) { $input.Path } elseif ($input.path) { $input.path } else { $null }
            if ($p) { return (Shorten-Text ([string]$p) 120) }
        }
        if ($tool -in @('bash', 'run_command')) {
            $c = if ($input.command) { $input.command } elseif ($input.CommandLine) { $input.CommandLine } elseif ($input.commandLine) { $input.commandLine } else { $null }
            if ($c) { return (Shorten-Text ([string]$c) 120) }
        }
    }
    if ($state -and $state.title) { return (Shorten-Text ([string]$state.title) 120) }
    if ($input) {
        foreach ($name in @('filePath', 'file_path', 'Path', 'path', 'command', 'CommandLine', 'commandLine', 'pattern', 'description', 'query', 'prompt')) {
            $v = $input.$name
            if ($v) { return (Shorten-Text ([string]$v) 120) }
        }
        try { return (Shorten-Text ($input | ConvertTo-Json -Compress -Depth 6) 120) } catch {}
    }
    return ""
}

# One event line -> "HH:mm:ss  <kind>  <short target>" (or $null for empty/uninteresting text).
# Non-JSON lines and unknown event types never throw; they become a raw/type line instead.
function Format-ProgressEvent {
    param([string]$Line)
    if ([string]::IsNullOrWhiteSpace($Line)) { return $null }
    $clock = (Get-Date).ToString("HH:mm:ss")
    $evt = $null
    try { $evt = $Line | ConvertFrom-Json -ErrorAction Stop }
    catch { return ("{0}  raw  {1}" -f $clock, (Shorten-Text $Line 160)) }
    if ($null -eq $evt) { return ("{0}  raw  {1}" -f $clock, (Shorten-Text $Line 160)) }

    if ($evt.timestamp) {
        try { $clock = [DateTimeOffset]::FromUnixTimeMilliseconds([long]$evt.timestamp).ToLocalTime().ToString("HH:mm:ss") }
        catch {}
    }

    # agy stream-json format (card 241)
    if ($evt.event) {
        $eventKind = [string]$evt.event
        if ($eventKind -eq 'init') {
            return ("{0}  init" -f $clock)
        }
        if ($eventKind -eq 'step_update') {
            $step = $evt.step_update
            if (-not $step) { return $null }
            $stepType = [string]$step.step_type

            if ($step.error) {
                return ("{0}  error  {1}" -f $clock, (Shorten-Text ([string]$step.error) 160))
            }
            if ($stepType -eq 'tool') {
                if ($step.state -eq 'DONE' -and -not $step.error) {
                    return $null
                }
                $tool = if ($step.tool_name) { [string]$step.tool_name }
                        elseif ($step.tool_info -and $step.tool_info.name) { [string]$step.tool_info.name }
                        elseif ($step.tool) { [string]$step.tool }
                        else { "tool" }
                $target = Get-EventTarget -Part $step
                if ($target) { return ("{0}  {1}  {2}" -f $clock, $tool, $target) }
                return ("{0}  {1}" -f $clock, $tool)
            }
            if ($stepType -in @('text', 'agent_response')) {
                $text = if ($step.text) { [string]$step.text }
                        elseif ($step.text_delta) { [string]$step.text_delta }
                        else { "" }
                if ([string]::IsNullOrWhiteSpace($text)) { return $null }
                return ("{0}  text  {1}" -f $clock, (Shorten-Text $text 160))
            }
            if ($stepType -eq 'user_input') { return $null }
            if (-not $stepType) { return $null }
            return ("{0}  {1}" -f $clock, $stepType)
        }
        if ($eventKind -eq 'result') {
            $res = $evt.result
            if ($res) {
                if ($res.status -eq 'ERROR' -or $res.error) {
                    $msg = if ($res.error) { [string]$res.error } else { "result error" }
                    return ("{0}  error  {1}" -f $clock, (Shorten-Text $msg 160))
                }
                $status = if ($res.status) { [string]$res.status } else { "SUCCESS" }
                return ("{0}  result  {1}" -f $clock, $status)
            }
            return ("{0}  result" -f $clock)
        }
        if ($eventKind -match 'error') {
            $msg = if ($evt.error) { [string]$evt.error } elseif ($evt.message) { [string]$evt.message } else { "unknown error" }
            return ("{0}  error  {1}" -f $clock, (Shorten-Text $msg 160))
        }
        return ("{0}  {1}" -f $clock, $eventKind)
    }

    $type = [string]$evt.type

    # claude stream-json format (claude -p --output-format stream-json --verbose): system / assistant
    # (content blocks: text, tool_use, thinking) / user (tool results) / result.
    if ($type -in @('system', 'assistant', 'user') -or ($type -eq 'result' -and $evt.subtype)) {
        if ($type -eq 'system') {
            if ($evt.subtype -eq 'init') { return ("{0}  init" -f $clock) }
            return $null
        }
        if ($type -eq 'user') { return $null }
        if ($type -eq 'result') {
            if ($evt.is_error -or $evt.subtype -ne 'success') {
                $msg = if ($evt.result) { [string]$evt.result } else { [string]$evt.subtype }
                return ("{0}  error  {1}" -f $clock, (Shorten-Text $msg 160))
            }
            return ("{0}  result  success" -f $clock)
        }
        $lines = New-Object System.Collections.Generic.List[string]
        foreach ($block in @($evt.message.content)) {
            if (-not $block) { continue }
            if ($block.type -eq 'tool_use') {
                $target = Get-EventTarget -Part $block
                if ($target) { $lines.Add(("{0}  {1}  {2}" -f $clock, $block.name, $target)) }
                else { $lines.Add(("{0}  {1}" -f $clock, $block.name)) }
            } elseif ($block.type -eq 'text' -and -not [string]::IsNullOrWhiteSpace([string]$block.text)) {
                $lines.Add(("{0}  text  {1}" -f $clock, (Shorten-Text ([string]$block.text) 160)))
            }
        }
        if ($lines.Count -eq 0) { return $null }
        return ($lines -join "`r`n")
    }

    # codex exec --json format: thread.started, turn.started/completed/failed, item.started/
    # updated/completed (item.type: agent_message, reasoning, command_execution, file_change,
    # mcp_tool_call, web_search, todo_list, error), error.
    if ($type -like '*.*') {
        if ($type -eq 'thread.started') { return ("{0}  init" -f $clock) }
        if ($type -eq 'turn.completed') { return ("{0}  result  success" -f $clock) }
        if ($type -eq 'turn.failed') {
            $msg = if ($evt.error -and $evt.error.message) { [string]$evt.error.message } else { "turn failed" }
            return ("{0}  error  {1}" -f $clock, (Shorten-Text $msg 160))
        }
        $item = $evt.item
        if (-not $item) { return $null }
        $itemType = [string]$item.type
        if ($type -eq 'item.started') {
            if ($itemType -eq 'command_execution') { return ("{0}  bash  {1}" -f $clock, (Shorten-Text ([string]$item.command) 120)) }
            if ($itemType -eq 'mcp_tool_call') { return ("{0}  {1}.{2}" -f $clock, $item.server, $item.tool) }
            if ($itemType -eq 'web_search') { return ("{0}  search  {1}" -f $clock, (Shorten-Text ([string]$item.query) 120)) }
            return $null
        }
        if ($type -eq 'item.completed') {
            if ($itemType -eq 'agent_message' -and -not [string]::IsNullOrWhiteSpace([string]$item.text)) {
                return ("{0}  text  {1}" -f $clock, (Shorten-Text ([string]$item.text) 160))
            }
            if ($itemType -eq 'file_change') {
                $files = @(@($item.changes) | Where-Object { $_ } | ForEach-Object { [string]$_.path }) -join ', '
                return ("{0}  edit  {1}" -f $clock, (Shorten-Text $files 120))
            }
            if ($itemType -eq 'command_execution' -and $null -ne $item.exit_code -and [int]$item.exit_code -ne 0) {
                return ("{0}  error  exit {1}: {2}" -f $clock, $item.exit_code, (Shorten-Text ([string]$item.command) 120))
            }
            if ($itemType -eq 'error') { return ("{0}  error  {1}" -f $clock, (Shorten-Text ([string]$item.message) 160)) }
        }
        return $null
    }

    # opencode format
    $part = $evt.part

    if ($type -eq 'tool_use') {
        $tool = if ($part -and $part.tool) { [string]$part.tool } else { "tool" }
        $target = Get-EventTarget -Part $part
        if ($target) { return ("{0}  {1}  {2}" -f $clock, $tool, $target) }
        return ("{0}  {1}" -f $clock, $tool)
    }
    if ($type -eq 'text') {
        $text = if ($part -and $part.text) { [string]$part.text } else { "" }
        if ([string]::IsNullOrWhiteSpace($text)) { return $null }
        return ("{0}  text  {1}" -f $clock, (Shorten-Text $text 160))
    }
    if ($type -match 'error') {
        $msg = ""
        foreach ($cand in @($part.error, $part.message, $part.text, $evt.error, $evt.message)) {
            if ($null -eq $cand) { continue }
            if ($cand -is [string]) { $msg = [string]$cand }
            elseif ($cand.message) { $msg = [string]$cand.message }
            else { try { $msg = ($cand | ConvertTo-Json -Compress -Depth 6) } catch {} }
            if ($msg) { break }
        }
        if (-not $msg) { $msg = "unknown error" }
        return ("{0}  error  {1}" -f $clock, (Shorten-Text $msg 160))
    }
    if ($type -eq 'step_start') { return ("{0}  step  start" -f $clock) }
    if ($type -eq 'step_finish') {
        $reason = if ($part -and $part.reason) { [string]$part.reason } else { "" }
        if ($reason) { return ("{0}  step  finish ({1})" -f $clock, $reason) }
        return ("{0}  step  finish" -f $clock)
    }
    if (-not $type) { $type = "unknown" }
    return ("{0}  {1}" -f $clock, $type)
}

# Last lines of a progress log + how long ago the newest line was written (ww-status display).
function Get-ProgressInfo {
    param([string]$Path, [int]$Tail = 5)
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return $null }
    $item = Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue
    if (-not $item) { return $null }
    $lines = @(Get-Content -LiteralPath $Path -Tail $Tail -ErrorAction SilentlyContinue)
    return [pscustomobject]@{
        Path       = $Path
        Lines      = $lines
        AgeMinutes = [math]::Round(((Get-Date) - $item.LastWriteTime).TotalMinutes, 1)
    }
}

# The "ReportsWritten" rule from ww-run, shared with the dead-record cleanup: reports touched
# since the run started whose name is phase-*.md or <Task>*.md.
function Get-ReportsWritten {
    param(
        [Parameter(Mandatory = $true)][string]$ReportsDir,
        $Started = $null,
        [Parameter(Mandatory = $true)][string]$Task
    )
    if (-not (Test-Path -LiteralPath $ReportsDir)) { return @() }
    $start = $null
    if ($Started -is [datetime]) { $start = $Started }
    elseif ($Started) { try { $start = [datetime]::Parse([string]$Started) } catch {} }
    $files = @(Get-ChildItem -LiteralPath $ReportsDir -File -ErrorAction SilentlyContinue)
    if ($start) { $files = @($files | Where-Object { $_.LastWriteTime -ge $start }) }
    return @($files | Where-Object { $_.Name -like "phase-*.md" -or $_.Name -like "$Task*.md" } | ForEach-Object { $_.Name })
}

function Test-ProcessAlive([int]$Id) {
    if ($Id -le 0) { return $false }
    return [bool](Get-Process -Id $Id -ErrorAction SilentlyContinue)
}

# Dead-record cleanup (card 190): a record is dead when its ww-run Host PID is gone AND no
# opencode.exe ... run command line names its card. A live Host is never touched.
# Called by ww-status on each refresh and by ww-run before its duplicate check. Read-only except
# for the dead record itself, so the self test can drive it with a fixture data folder.
function Clear-DeadRunRecords {
    param(
        [Parameter(Mandatory = $true)][string]$DataDir,
        [string]$ScriptName = "ww",
        $Runs = $null
    )
    $runDir = Join-Path $DataDir "running"
    if (-not (Test-Path -LiteralPath $runDir)) { return @() }
    if ($PSBoundParameters.ContainsKey('Runs')) {
        $active = @(@($Runs) | Where-Object { $_ })
    } else {
        $active = @(Get-WorkerRuns)
    }
    $reportsDir = Join-Path $DataDir "reports"
    $legacy = Join-Path $DataDir "running.json"
    $now = Get-Date
    $cleaned = New-Object System.Collections.Generic.List[string]

    $files = @(Get-ChildItem -LiteralPath $runDir -Filter "*.json" -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notlike "*.last.json" })
    foreach ($file in $files) {
        $record = $null
        try { $record = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json } catch { continue }
        if (-not $record -or -not $record.Task) { continue }
        $task = [string]$record.Task

        $hostPid = 0
        if ($record.Host) { [void][int]::TryParse([string]$record.Host, [ref]$hostPid) }
        if (Test-ProcessAlive -Id $hostPid) { continue }
        $named = @($active | Where-Object { Test-CommandLineHasTask -CommandLine $_.CommandLine -Task $task })
        if ($named.Count -gt 0) { continue }

        $last = @{}
        foreach ($p in $record.PSObject.Properties) { $last[$p.Name] = $p.Value }
        $last.Ended = $now.ToString("s")
        $last.ExitCode = $null
        $last.Dead = $true
        $last.ReportsWritten = @(Get-ReportsWritten -ReportsDir $reportsDir -Started $record.Started -Task $task)
        $last | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 (Join-Path $runDir "$task.last.json")

        if ($record.Log) {
            try {
                $append = "ended (dead record cleaned by {0} at {1}){2}" -f $ScriptName, $now.ToString("s"), "`r`n"
                [System.IO.File]::AppendAllText($record.Log, $append, (New-Object System.Text.UTF8Encoding($false)))
            } catch {}
        }
        Remove-Item -LiteralPath $file.FullName -ErrorAction SilentlyContinue

        if (Test-Path -LiteralPath $legacy) {
            try {
                $l = Get-Content -LiteralPath $legacy -Raw | ConvertFrom-Json
                if ([string]$l.Task -eq $task) { Remove-Item -LiteralPath $legacy -ErrorAction SilentlyContinue }
            } catch {}
        }
        Write-Host ("[{0}] cleaned dead run record for task {1} (host PID {2} not alive)" -f $ScriptName, $task, $record.Host) -ForegroundColor Yellow
        $cleaned.Add($task)
    }
    return @($cleaned)
}

# Find the worker (agy or opencode) spawned for this run (best effort; the record stores it when known).
function Find-WorkerPid {
    param([Parameter(Mandatory = $true)][string]$Task, [int]$Attempts = 8, [int]$DelayMs = 250)
    for ($i = 0; $i -lt $Attempts; $i++) {
        $cand = @(Get-WorkerRuns |
            Where-Object { Test-CommandLineHasTask -CommandLine $_.CommandLine -Task $Task } |
            Sort-Object CreationDate -Descending)
        if ($cand.Count -gt 0) { return [int]$cand[0].ProcessId }
        Start-Sleep -Milliseconds $DelayMs
    }
    return $null
}

function Find-OpencodePid {
    param([Parameter(Mandatory = $true)][string]$Task, [int]$Attempts = 8, [int]$DelayMs = 250)
    Find-WorkerPid -Task $Task -Attempts $Attempts -DelayMs $DelayMs
}

# Worktree pre-flight (card 170): a card may edit a path that does not exist in a fresh
# worktree because it is untracked/ignored in git (e.g. a gitignored vendor folder before card
# 150). For every listed path that exists in the main checkout, ask git whether its parent
# folder is tracked; if not, the worktree would be missing files the card is allowed to touch.
function Get-UntrackedAuthorisedPaths {
    param(
        [Parameter(Mandatory = $true)][string]$CardPath,
        [Parameter(Mandatory = $true)][string]$RepoRoot
    )
    $bad = New-Object System.Collections.Generic.List[string]
    foreach ($rel in @(Get-AuthorisedEditPaths -CardPath $CardPath)) {
        $full = Join-Path $RepoRoot $rel
        if (-not (Test-Path -LiteralPath $full)) { continue }
        $folder = if (Test-Path -LiteralPath $full -PathType Container) { $full } else { [System.IO.Path]::GetDirectoryName($full) }
        if (-not $folder) { continue }
        # git writes the "unknown path" case to stderr; keep that non-terminating here even
        # when the caller (ww-run) runs with $ErrorActionPreference = Stop.
        $previous = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        & git -C $RepoRoot ls-files --error-unmatch -- $folder 2>&1 | Out-Null
        $tracked = ($LASTEXITCODE -eq 0)
        $ErrorActionPreference = $previous
        if (-not $tracked -and -not $bad.Contains($rel)) { $bad.Add($rel) }
    }
    return @($bad)
}
