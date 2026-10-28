# LLM JSON Stream Format (pcder -> ProcessCommander)

ProcessCommander (`pcder.exe` / ProcessCommander) can parse JSON Lines (JSONL) emitted by LLM coding agents and record structured events to `<logfile>.stream.jsonl` and the live stream view.

## Enabling

- Automatic: when launching via `pcder start` and `agent_type` is detected (`claude`, `agy`/`antigravity`, `opencode`, `codex`, `aider`, `ollama`) or `json_stream` is true. `StartProcessRequest.jsonStream`/`--json-stream` may also be used.
- The parser is in `src/llm_stream.cpp` / `src/llm_stream.h` (`ParseLlmStreamLine`, `LlmStreamTracker`).
- Detected agent types: `DetectAgentType(commandLine, name)` looks for `claude`, `agy`/`antigravity`, `opencode`, `codex`, `aider`, `ollama`.

## Parsed LLM Stream Event (`LlmStreamEvent`)

| Field | Type | Notes |
|---|---|---|
| `seq` | uint64 | Monotonic sequence number assigned by tracker |
| `timestamp` | string | ISO-like/time string; filled by tracker if missing |
| `rawJson` | string | Original trimmed JSON line |
| `type` | string | Normalized: `tool_call`, `tool_result`, `thinking`, `message`, `status`, `step`, `error`, `usage`, `other`, or fallback to raw `type` |
| `agentType` | string | Agent type from event or default (`claude`/`agy`/`opencode`/`codex`/`generic`) |
| `model` | string | Model id/name if present in stream (see below) |
| `toolName` | string | Tool name (e.g. `view_file`, `edit_file`, `Bash`, Claude `tool_calls[].name`/`function.name`) |
| `toolAction` | string | Human-readable action/summary |
| `toolInput` | string | JSON string of arguments/parameters/input |
| `content` | string | Thinking/reasoning/text content |
| `status` | string | `thinking`, `calling_tool`, `generating`, `idle`, `completed`, `error`, etc. |
| `stepIndex` | int | From `step_index` or `step` |
| `totalTokens` | uint64 | If present |
| `costUsd` | double | If present |

## Agent State (`LlmAgentState`)

| Field | Type | Notes |
|---|---|---|
| `agentType` | string | Current agent type |
| `model` | string | Current model if observed in stream |
| `status` | string | Aggregated status (`idle`/`thinking`/`calling_tool`/`tool_finished`/`generating`/`completed`/`error`) |
| `currentTool`/`currentToolAction` | string | Active tool info |
| `lastThought`/`lastMessage`/`lastError` | string | Truncated previews |
| `stepCount` | int | Latest step index |
| `eventCount` | int | Count of stored events |
| `totalTokens`/`costUsd` | ... | Aggregated if present |
| `streamFilePath` | string | Path to `.stream.jsonl` |

## Recognized JSON Shapes

The parser is permissive and looks for common fields across agents:

### Agent/model hints
- `agent` (string)
- `model`, `modelId`, `model_name`
- `llm.model` / `llm.name`
- `metadata.model`

### Tool calls
- Claude-style: top-level `tool_calls` array; or `type` like `tool_use`/`tool_call`/`function_call`. Names from `toolName`/`name`/`tool`/`function.name`; inputs from `parameters`/`arguments`/`input`.
- Some streams use `PLANNER_RESPONSE` with `tool_calls`.

### Tool results
- `type` in `tool_result`/`function_result`/`tool_response`; content from `content`/`output`/`result`.

### Thinking/reasoning
- `type` in `thinking`/`thought`/`reasoning` or fields `thinking`/`thought`/`reasoning`.

### Messages/deltas
- `type` in `message`/`assistant`/`user`/`content_block_delta`/`delta`; content from `content`/`text`/`message`/`delta.content`/`delta.text`.

### Status/error/result
- `type` in `status`/`error`/`result`/`done`/`finish`/`completed`. `error` may be string or object with `message`.

## Outputs

- **Per-process stream file**: `<logfile>.stream.jsonl` (written by `LlmStreamTracker` as raw JSON lines are ingested).
- **GUI**: Stream dialog shows events list and preview; agent icon shows tooltip with model when known.
- **IPC/MCP**: `GetAgentStream`/IPC and MCP endpoints can expose events/state (see `ipc_server.cpp`, `mcp_server.cpp`).

## Notes

- Non-JSON lines are ignored by the parser.
- The tracker caps stored events (`kMaxEvents`) and persists raw lines for post-mortem.
- Model capture is best-effort from stream content; ProcessCommander cannot reliably infer the LLM model from process metadata alone.
