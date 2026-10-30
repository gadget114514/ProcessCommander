# タスクキュー: 順序・依存・タイムアウト・自動再実行

**[English](taskqueue.md) | [日本語 (Japanese)](taskqueue-ja.md)**

ProcessCommander がワークフローを持ちます。ジョブと依存を宣言すると、実行順・同時実行数・タイムアウト・成功判定・再実行を PC が行います。

**1 ジョブ = 1 コマンド。** `A && B && C` は書けません。3 つのジョブを登録し、依存を宣言します:

```json
{ "taskqueue": "main", "concurrency": 1,
  "jobs": [
    { "id": "A", "command": "run A", "summary": "Prepare inputs", "timeoutSec": 1800 },
    { "id": "B", "command": "run B", "summary": "Build outputs", "timeoutSec": 3600, "dependsOn": ["A"] },
    { "id": "C", "command": "run C", "summary": "Publish", "timeoutSec": 900,
      "dependsOn": [{ "id": "B", "on": "success" }] }
  ] }
```

```powershell
pcder.exe taskqueue add --file jobs.json --taskqueue main
pcder.exe taskqueue list main
pcder.exe taskqueue graph main
pcder.exe taskqueue wait main --since 0 --timeout-ms 30000
```

**ジョブのフィールド**

| フィールド | 意味 |
| :--- | :--- |
| `id`, `command` | ジョブ id（全キューで一意）と、ちょうど 1 つのコマンド。 |
| `summary` (必須) | 1 行・200 文字以内。`{summary}` に置換され、`PC_JOB_SUMMARY` にも入る。 |
| `timeoutSec` (必須) | ジョブ単位の制限。合算しない。キューの `maxTimeoutSec`（既定 7200）以下。 |
| `dependsOn` | `["A"]`（= `on: success`）または `[{ "id": "A", "on": "done" }]`。別キューの id も可。 |
| `idle` | `{ "outputSec": 720, "cpuSec": 720 }`。両方指定必須（出力と CPU の両方が止まったときだけ kill）。 |
| `maxAttempts`, `retryDelaySec` | 自動再実行（既定 2）と試行間の待ち。 |
| `success` | `exitCode`（既定 0）AND `filesNewer`（存在し、試行開始より新しい）AND `outputRegex`（ログ末尾 200 行）。 |
| `cwd`, `env`, `killExclude`, `priority` | 作業ディレクトリ、追加環境変数、ツリー kill 除外イメージ名、優先度。 |
| `resources` | `{ "write": [...], "read": [...] }`。パス/glob（`cwd` 基準）または `@name` ロック。集合が重なる（write/write、write/read）ジョブはキューをまたいでも同時に実行しない。read 同士は並列可。デフォルトはオン（設定画面のチェックボックス）。[resource-locks-ja.md](resource-locks-ja.md) を参照。 |
| `instructionsFile` / `instructionsText` | 任意の指示書。ジョブに複製され、版ごとに実体化される。 |

**状態**

| 状態 | 意味 |
| :--- | :--- |
| `waiting` | キュー済みで実行待ち（空きスロットまたは再実行待ち）。 |
| `pending` | まだ実行キューに入っていない（依存が未達）。 |
| `blocked` | success 依存が失敗/キャンセル。依存を直して `taskqueue_retry` すると blocked の後続も自動で戻る。 |
| `running` | 試行（1 つの managed process）が実行中。 |
| `succeeded` / `failed` / `cancelled` | 終了状態。failed は `maxAttempts` まで自動再実行。 |

**ChainGuard:** `start_process` / `taskqueue_add` / `taskqueue_update` は `&&`, `||`, 単独 `&`, PowerShell `;`, 改行連鎖, 待ちループ（`while` + `Test-Path`, `goto` ループ）を拒否します。コマンドをジョブに分割するのが正しい対処です。CLI の `--allow-chain` は単一プロセス用の例外で、MCP には公開しません。

**イベントと環境変数:** 各試行に `PC_JOB_ID`, `PC_ATTEMPT`, `PC_JOB_SUMMARY`, `PC_JOB_INSTRUCTIONS`, `PC_JOB_INSTRUCTIONS_REV`、再実行時は `PC_PREV_REASON`, `PC_PREV_EXIT`, `PC_PREV_TAIL`。イベントは `logs/taskqueue-<name>.events.jsonl` に追記され、`notifyFile` は `jobFailed` / `taskqueueStalled` / `taskqueueDrained` で更新されます。

**GUI:** 左端の表示切替ボタン、または **Ctrl+T** で **2行同時表示 (Two Rows)**、**プロセスのみ (Processes Only)**、**タスクのみ (Tasks Only)** を順次トグル切り替え。2行表示時は水平スプリッターバーをドラッグして各領域の高さを自在に調整できます。実行中は上位・強調表示、失敗は低彩度の赤、タイムアウトは低彩度のアンバー（テーマ色）。
