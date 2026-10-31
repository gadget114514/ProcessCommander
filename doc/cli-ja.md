# 使い方 1: CLI コマンドとして呼び出す

**[English](cli.md) | [日本語 (Japanese)](cli-ja.md)**

CLI コマンドは Named Pipe (`\\.\pipe\ProcessCommander_IPC_v1`) を通じて起動中の GUI ダッシュボードと連携します（GUI 未起動時は自動で起動します）。

> **呼称:** **PCR** は **ProcessCommander**（GUI ダッシュボード / MCP サーバー、`ProcessCommander.exe`）の略称です。ドキュメント・プロンプト・ジョブの notes などで「PCR」と書かれていれば ProcessCommander を指します。`pcder.exe` はそのコンソールクライアントです。両方の help の `NAMING` 節に記載されており、`help --json` では `aliases.PCR` として取得できます。

## バージョン確認（`--version` / `-V` / `version`）

```powershell
pcder.exe --version              # pcder 2.0.1
pcder.exe version --json         # {"ok":true,"program":"pcder","version":"2.0.1"}
ProcessCommander.exe --version   # ProcessCommander 2.0.1
ProcessCommander.exe version --json  # {"ok":true,"program":"ProcessCommander","version":"2.0.1"}
```

バージョンは両方の help の 1 行目、`pcder.exe help --json` の `version`、MCP の `initialize` 応答の `serverInfo.version` にも出力されます。

```powershell
# GUI ダッシュボードの起動（トレイ格納中の場合は前面に復元）
ProcessCommander.exe gui

# ConPTY モードでプロセスを起動（ログ出力先と完了検知用 .done ファイルを指定）
ProcessCommander.exe start --name "agent-task" --pty --cmd "opencode run \"Fix issue #42\"" --log-file "C:/logs/task42.log" --done-file "C:/logs/task42.done"

# 全プロセスの稼働状況一覧を取得 (JSON)
ProcessCommander.exe list

# 指定プロセスの最新出力ログを取得
ProcessCommander.exe logs --id 1 --tail 80

# 実行中プロセスの ConPTY / stdin へ入力を送信
ProcessCommander.exe send --id 1 --input "y"

# プロセスの終了待機 / 強制終了 / 終了履歴クリア
ProcessCommander.exe wait --id 1 --wait-ms 30000
ProcessCommander.exe kill --id 1
ProcessCommander.exe kill-all
ProcessCommander.exe clear
```

`start` は既定で **pipe モード**（ConPTY 非経由）でプロセスを起動します。対話型ターミナルとして ConPTY を使う場合は `--pty`（または `--mode pty`）、別ウィンドウのコンソールを使う場合は `--mode console` を指定してください。MCP の `start_process` も同様に `mode` の既定は `pipe` で、`pty: true` を指定すると ConPTY が有効になります。

pipe モードでは既定で stdin が書き込み可能なままなので、`pcder.exe send` で入力を送れます。`opencode run` のような非対話プログラムは stdin の EOF を待ち続けることがあるため、その場合は `--close-stdin` を指定して起動直後に stdin を閉じてください:

```powershell
# pcder.exe send で入力を送るため stdin を開いたままにする
pcder.exe start --name "pipe-shell" --cmd "cmd.exe"

# stdin を読む非対話プログラム: 閉じて EOF を届ける
pcder.exe start --name "oc-run" --close-stdin --cmd "opencode run OK" --timeout 90
```

`opencode`（`run` なしの TUI）のような対話型プログラムは `--pty` を推奨します。MCP では `open_stdin: false` が `--close-stdin` に相当します。

稼働中は **End Time** 列に見積終了時刻がグレーで表示されます。`--eta "18:30"`（または `--estimate-end`）で明示的に指定でき、省略時は `--timeout` に基づく残り秒数（例: `残り 120 秒`）を表示します。

`--cwd` を省略した場合、`pcder.exe` を実行したディレクトリが使用され、絶対パスに解決されます。ProcessCommander は子プロセスの実際の作業ディレクトリと `PWD` / `INIT_CWD` をそのパスに揃え（`OLDPWD` は削除）、opencode のように `PWD` からプロジェクトディレクトリを決定するツールでも意図したフォルダを利用できるようにします。

**完了通知:** プロセス終了時に `.done` JSON ファイルがアトミックに書き出されます。完了まで待つ場合は `pcder.exe start --wait`（必要なら `--wait-ms` で上限指定）、MCP の `start_process` で `"wait": true`、または `wait_process` を使用してください。`wait` を指定しない場合、`start` は即座に戻るため、`.done` ファイルをポーリングするか `wait` を呼び出す必要があります。

**Claude Code のバックグラウンドタスク:** `run_in_background` でブロッキング起動します:

```powershell
pcder.exe start --name "task" --cwd C:\ws\Survivors --close-stdin --timeout 3600 --wait --wait-ms 0 --json --cmd C:\path\run_task.cmd
```

`pcder.exe` は管理対象プロセスが終了するまで残るため、`pcder.exe` の終了時に Claude Code が `<task-notification>` を出します。終了コードは完了時 `0`、失敗時は子プロセスの終了コード、待機タイムアウト時は `124` です。出力ファイルには最終 JSON（`state`, `exit_code`, `log_file`, `done_file`）が残ります。

**タスクキュー（順序・依存つきジョブ）:**

```powershell
# jobs.json: 1 ジョブ 1 コマンド、それぞれに timeoutSec
# [{"id":"A","command":"run A","summary":"Prepare","timeoutSec":1800},
#  {"id":"B","command":"run B","summary":"Build","timeoutSec":3600,"dependsOn":["A"]}]
pcder.exe taskqueue add --file jobs.json --taskqueue main --concurrency 1
pcder.exe taskqueue list main
pcder.exe taskqueue graph main
pcder.exe taskqueue wait main --since 0 --timeout-ms 30000
pcder.exe taskqueue retry A
```

**プロジェクト内のデータディレクトリ（git 管理）:**

```powershell
pcder.exe --data-dir .processcommander taskqueue add --file jobs.json
pcder.exe taskqueue list main --data-dir .processcommander
pcder.exe mcp install --data-dir .processcommander
```

**MCP サーバーの自動登録:**

```powershell
pcder.exe mcp install                     # .mcp.json (Claude Code)
pcder.exe mcp install --client opencode   # opencode.json
pcder.exe mcp install --dry-run --json    # 書き込まずに確認
```

**pc-runner サブエージェント（Claude Code）を配置する:**

```powershell
pcder.exe agent install-template                  # .claude\agents\pc-runner.md を書く
pcder.exe agent install-template --path C:\proj --dry-run
```

`pc-runner` は長いビルドやテストを ProcessCommander の MCP ツールで実行し、要約だけを返す。既存のファイルは上書きしない。
