# ProcessCommander: Pros/Cons Comparison

Pros/Cons analyses for choosing how to run work, comparing ProcessCommander with LLM-native sub-agents and other overlapping technologies.

Back to [README](../README.md).

---

## Sub-Agent vs. ProcessCommander Execution

A short Pros/Cons analysis for choosing how to run work: **LLM-native sub-agents** vs. **ProcessCommander-managed CLI processes**.

### Sub-Agent Execution (LLM-native)

**Pros**
- Deep integration with the supervising LLM: shares context, reasons adaptively, uses tools, and can change plans mid-flight.
- No extra binary or setup; works wherever the LLM client runs.
- Ideal for short, reasoning-heavy, code-editing and exploratory tasks.
- Natural error diagnosis and retry with human-like judgment.

**Cons**
- Token- and cost-heavy for large/repetitive jobs; bounded by the context window.
- Poor fit for long-running, interactive, or process-tree-heavy workloads (builds, dev servers, TUI agents).
- Limited observability: no live terminal grid, idle detection, or job-object tree control.
- Concurrency and dependency orchestration are client-dependent and often implicit.
- Hard to persist or recover state across LLM session restarts.

### ProcessCommander Execution (external CLI processes)

**Pros**
- Runs real CLI agents/tools (`claude`, `opencode`, `codex`, build scripts) as managed OS processes with Win32 Job Object tree control.
- Live dashboard: up to 16 terminals, idle detection, per-job timeouts, retries, dependencies, and persisted task queues.
- `.done` markers and per-process logs enable simple, robust completion detection by LLMs/automation.
- Lower token cost for heavy work; the LLM acts as supervisor only.
- MCP + CLI integration and project-local data directories.
- State recovery across restarts.

**Cons**
- Windows-only; requires installation and setup.
- Processes are comparatively opaque: the LLM sees captured output, not internal reasoning.
- Task specs must be explicit (command, timeout, success checks).
- Adds operational complexity and resource overhead.
- Interactive debugging requires attaching via the dashboard.

### Recommendation

Use **sub-agents** for reasoning and code changes; use **ProcessCommander** for parallel, long-running, interactive, or build-heavy CLI workloads. Best of both: let the LLM supervisor delegate heavy jobs to ProcessCommander while sub-agents handle analysis and edits.

---

## Other Alternatives vs. ProcessCommander

Pros/Cons of the alternatives that overlap most with ProcessCommander. Each list describes the alternative; the comparison line states where ProcessCommander differs.

### Claude Code Built-in Background Execution (`run_in_background`, Monitor)

**Pros**
- Zero setup: built into the LLM client, nothing to install.
- Completion notifies the LLM automatically; no polling or `.done` markers needed.
- Cross-platform wherever the client runs.

**Cons**
- Tied to the session: background jobs and their state do not survive a client restart.
- No dashboard; output is visible only through the LLM.
- No dependencies, priorities, retries, idle detection, or success checks.
- Interactive input (ConPTY, TUI agents) is not supported.
- Process-tree cleanup depends on the client.

**vs. ProcessCommander:** Use built-in background execution for a few one-off long commands within a session. Use ProcessCommander when jobs must outlive the session, form a dependency graph, run interactively, or be watched live.

### tmux / screen-based Agent Managers (e.g. claude-squad)

**Pros**
- De facto standard on Unix; mature, lightweight, scriptable.
- Humans can attach to any agent's terminal and take over interactively.
- Agent managers often pair each agent with a git worktree for isolation.
- Sessions survive terminal disconnects (and SSH).

**Cons**
- Not Windows-native (needs WSL/MSYS); Windows tools and toolchains are awkward to drive.
- No MCP interface: a supervising LLM must scrape panes with `send-keys` / `capture-pane`.
- No structured output (JSON), completion markers, timeouts, retries, or dependencies.
- No job-object-style process-tree control.

**vs. ProcessCommander:** ProcessCommander offers the same "many live agent terminals" model natively on Windows, plus MCP/JSON control and a task queue. tmux remains the better fit on Linux/macOS and over SSH.

### Claude Agent SDK / Headless `claude -p` / LangGraph (code-orchestrated agents)

**Pros**
- Fine-grained programmatic control: prompts, tools, permissions, structured (JSON) results.
- Orchestration logic (branching, loops, state graphs) is code that can be versioned and tested.
- Cross-platform; easy to embed in services and CI.

**Cons**
- Process management, logging, timeouts, retries, and recovery must be written by hand.
- No built-in live view of running agents; observability is whatever you build.
- Workers usually run in-process or headless, so humans cannot attach and steer them interactively.
- Tied to one SDK/vendor; arbitrary third-party CLI agents need custom wrappers.

**vs. ProcessCommander:** The SDKs build orchestration into an application; ProcessCommander runs existing CLI agents and tools as observable OS processes without new code. They combine well: start SDK- or `claude -p`-based scripts as ProcessCommander jobs to gain dependencies, retries, persistence, and the dashboard.
