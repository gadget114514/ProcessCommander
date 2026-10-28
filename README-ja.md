# ProcessCommander

**[English](README.md) | [日本語 (Japanese)](README-ja.md)**

**ProcessCommander** は、Claude Code や各種自動化スクリプトから複数のサブプロセス・対話型 CLI エージェント（`opencode`, `claude`, PowerShell, cmd, Git Bash 等）を並列起動・監視・制御し、**状況をひと目で把握できる Native Win32 + Direct2D マルチターミナルダッシュボード GUI** を備えたプロセスマネージャです。

![ProcessCommander Dashboard](docs/images/dashboard.jpg)

---

## LLM エージェント向けクイックスタート

```powershell
pcder.exe mcp install --client opencode   # Claude Code は --client claude（.mcp.json）
pcder.exe help --json                     # 機械可読ガイド（agent.quick_start）
```

- **1 ジョブ = 1 コマンド。** 複数ステップや依存は `taskqueue_add` の `dependsOn`。シェルの `&&` / `||` / `&`、PowerShell の `;`、待ちループは拒否されます。
- **タイムアウトはジョブごと**（`timeoutSec`）。合算しません。1 行の `summary` も必須です。
- **成功は検証**されます（exit code + 任意の `filesNewer` / `outputRegex`）。
- **状態**: `waiting` = キュー済みで実行待ち、`pending` = 依存未達で未キュー、`blocked` = 依存の失敗。`taskqueue_wait` / `taskqueue_graph` で観測し、原因を直して `taskqueue_retry` で復帰します。
- 詳細は `pcder.exe help --json` の `agent`（`working_model` / `data_dir_consistency` / `decision` / `states` / `quick_start`）を参照してください。

作業構造: **監督 LLM（計画・宣言・観測）→ ProcessCommander（実行・タイムアウト・ログ・再実行）→ 子プロセスのワーカー LLM / ビルドコマンド（1 ジョブ 1 コマンド）**。監督は作業自体を実行せず、コマンドを連鎖せず、成功を偽装しません。

---

## プロジェクト内データディレクトリ (git 管理)

ログ・プロセス状態・タスクキュー・GUI 設定は 1 つの**データディレクトリ**に保存されます。`--data-dir <path>` で**呼び出しごと**に指定できます:

```powershell
pcder.exe --data-dir .processcommander taskqueue add --file jobs.json
pcder.exe taskqueue list main --data-dir .processcommander
pcder.exe mcp install --data-dir .processcommander   # プロジェクトの MCP 設定に埋め込む
```

一貫性の保証方法:

1. プロジェクトごとに 1 つのディレクトリ（例: `.processcommander/`）を決める。
2. `pcder.exe mcp install --data-dir .processcommander` を一度実行すると、`.mcp.json` / `opencode.json` に `--data-dir .processcommander` が書き込まれ、そのプロジェクトの MCP 呼び出しはすべて同じディレクトリを使う。
3. 直接 `pcder.exe` を呼ぶときは**毎回同じ `--data-dir`** を渡す（グローバル設定ではない）。
4. 相対パスは呼び出し時の cwd 基準で解決される。MCP 側の cwd が不明なら MCP 設定に絶対パスを書く。
5. データディレクトリごとにダッシュボードインスタンス・Named Pipe・ミューテックスが分離され、複数プロジェクトを同時に運用してもプロセス/キューを共有しない。
6. 確認: `pcder.exe --data-dir <path> help --json` の `effective_data_dir`、またはダッシュボードのステータスバー。

環境変数 `PROCESS_COMMANDER_LOG_DIR` も同等の指定方法です。

---

## 主な特徴

### 1. 一覧性と操作性を両立した Native Win32 + Direct2D ダッシュボード GUI
- **上部 KPI サマリーカード (2行・計14枚: プロセス & タスク)**:
  - **省スペース横並びレイアウト**: ラベルと数値を同一行に横並び配置し、ヘッダ高さを大幅短縮。プロセス一覧やターミナルの表示領域を最大化。
  - **上段 (プロセス)**: 管理ターミナル/プロセス総数、稼働中 (`ACTIVE RUNNING`)、入力待ち/アイドル (`WAITING / IDLE >= 10s`、出力もCPU消費もない場合)、正常完了 (`COMPLETED`)、失敗/強制終了 (`FAILED / KILLED`)、**最短残り時間** (最も期限が近いプロセスの残り秒数カウントダウン表示)、全 Job Object の合計メモリ使用量・プロセスツリー数をリアルタイム表示。
  - **下段 (タスクキュー)**: タスク総数、稼働中タスク (`RUNNING`)、待機/保留中タスク (`WAITING / PENDING`)、成功タスク (`SUCCEEDED`)、失敗タスク (`FAILED`)、**最短残り時間** (最も期限が近いタスクの残り秒数カウントダウン表示)、アクティブキュー数 & 最大並列度をリアルタイム表示。
  - 各カードにマウスカーソルを合わせると内訳ツールチップを表示。
- **2行スタック表示 & ドラッグ可能なスプリッター (プロセス一覧 ＋ タスクキュー一覧)**:
  - プロセス状況一覧（上段）とタスクキュー一覧（下段）を上下2段で同時に表示。
  - 境界線のスプリッターバーをマウスドラッグして各領域の高さを自由に調整可能。
  - ツールバーの表示切替ボタン（または **Ctrl+T**）で **2行同時表示 (Two Rows)** → **プロセスのみ (Processes Only)** → **タスクのみ (Tasks Only)** を順次トグル切り替え。
- **全プロセス状況リスト（新しい順・全カラムソート対応）**:
  - 新しく起動したプロセスほど**リストの一番上**に追加されます（デフォルト: `ID ▼` 降順）。
  - リスト上部の**任意のカラムヘッダ**（`ID`, `Agent / Name`, `Status`, `Start Time (起動時刻)`, `End Time (終了時刻)`, `Uptime (稼働時間)`, `PID (Tree)`, `Memory`, `CPU Time`, `Idle`, `Latest Output Preview`, `Command Line`）をクリックすると、**昇順 (`▲`) / 降順 (`▼`)** で自在にソートを切り替えられます。
  - **CPU 消費を考慮したアイドル判定**: CPU 時間が増え続けているプロセスは `RUNNING` として扱い、`IDLE / WAITING` 表示にせず、`--idle-timeout` の対象にもしません。CPU 時間は Win32 Job Object ツリー全体の合計です。
- **タブ切り替え式フルサイズ Direct2D ターミナル (`80桁×25行以上保証`)**:
  - Direct2D / DirectWrite / IME / Sixel 対応のフル VT ターミナルエミュレータ（`TerminalView` + `ConPtySession`）を搭載。
  - ターミナル領域上部の**タブバー**で表示するターミナルを切り替え、常に **80桁×25行以上** のコンソールサイズを確保するため、TUI アプリケーション（`claude`, `opencode`, `vim` 等）の表示崩れが起きません。
  - ターミナル画面内を直接クリックしてキー入力・`Ctrl+C`・日本語 IME 変換・マウスホイールスクロール・範囲選択コピーが可能です。
- **最大16並列対応のライブグリッドビュー (`[Grid View]`)**:
  - ツールバーの **`[Grid View]`** ボタンをクリックすると、並列稼働中のターミナル（最大16個・4×4グリッド）をサムネイル一覧表示します。任意のセルをクリック（または矢印キー＋Enter）すると、該当ターミナルのタブへ即座に切り替わります。

![Terminal Grid View](docs/images/grid_view.png)

### 2. `[Config]` ボタンによるシェル起動設定 (`cmd` / `powershell` / `bash`)
- ツールバーの **`[+ Launch (...)]`** ボタンで、設定中のデフォルトシェルを即座に ConPTY タブとして起動します。
- **`[設定]`** → **`[シェル構成...]`** からシェル設定ダイアログを開き、**`cmd`** (`cmd.exe`)、**`powershell`** (`powershell.exe`)、**`bash`** (`Git Bash` / `WSL`) のデフォルト切り替え・起動コマンドの編集・その場での個別起動が可能です（設定は `logs/config.json` に永続化されます）。

![Shell Config Dialog](docs/images/config_dialog.png)

### 3. 終了履歴の管理、タスク詳細・LLM JSON ツリービューア
- **`[終了履歴を非表示 (Hide Ended)]`**: 履歴データを消さずに、終了したプロセスをリスト・タブから非表示にして「実行中のプロセスのみ」を絞り込み表示します（再度クリックで再表示）。
- **`[終了履歴を消去]` & `[プロセスIDをリセット]`** (設定ダイアログ): 完了・強制終了したプロセスの履歴消去や、プロセスIDの再付番を実行できます。
- **右クリックメニュー & タスク詳細ダイアログ**:
  - リストの行またはタブを**右クリック**（または行をダブルクリック）すると、**「ツリー終了 (Kill Tree)」**（確認ダイアログ付き）、詳細表示、コマンドラインコピー、ログ/ストリームファイル閲覧、履歴削除を実行できます。
- **LLM JSON ストリーミング出力対応 & JSON ツリービューア**:
  - 主要 AI コーディングエージェント (`claude`, `agy`, `opencode`, `codex`) の JSON ストリーミング出力を自動検知・パースし、通常のコンソールログに加えて構造化 `.stream.jsonl` ファイルへリアルタイム記録。
  - タスク詳細ダイアログに `nlohmann/json` 採用の **JSON ツリービューア** を統合。生の JSONL 行を追うことなく、階層構造（オブジェクト、配列、ツール呼び出し引数、思考ブロック、トークン消費量など）を展開・折りたたみしながら整理されたツリー形式で直感的に確認・検証可能。

### 4. タスクトレイ格納・復元対応
- ツールバーの **`[トレイに格納 (Tray)]`** ボタン、最小化ボタン、またはウィンドウ右上の閉じるボタン（`[X]`）で、バックグラウンドで全プロセスと IPC/MCP 通信を維持したまま **タスクトレイ（通知領域）** に格納されます。
- タスクトレイアイコンの**左クリック / ダブルクリック**（または右クリックメニュー）でいつでも元の画面に拡大・復元できます。完全に終了する場合はトレイアイコンの右クリックメニューから「ProcessCommander を完全終了 (Exit)」を選択します。

### 5. リアルタイムログ保存・状態復元・完了検知 `.done` ファイル出力
- **プロセス別リアルタイムログ**: すべてのプロセスの出力は `logs/proc_<id>_<name>_<YYYYMMDD_HHMMSS>.log`（または `--log-file <path>`）にリアルタイム保存されます。
- **完了検知用 `.done` マーカーファイル**: プロセス終了時に `logs/proc_<id>.done`（または `--done-file <path>`）へ終了コード・稼働時間・ログパスを含む JSON をアトミックに出力します。Claude 等からファイルの存在チェックだけで確実に終了を検知できます。
- **再起動時の状態復元**: プロセス一覧と終了履歴は `logs/processes_state.json` に自動保存され、次回起動時に復元されます。

### 6. 設定ダイアログ・テーマ・ローカライゼーション
- **`[設定]`** ボタンで設定ダイアログを開きます。
- **言語**: **日本語 / English / Español** ボタンで即座に切り替え。ダッシュボード、ダイアログ、メニュー、ステータスバーが連動します。
- **テーマ (色)**: **Windows**（既定。Windows のライト/ダークモードと個人設定のアクセントカラーに追従）、**ダーク**、**ライト**、**ハイコントラスト** から選択。ボタン、リストヘッダ、スクロールバー、タブラベルもダークテーマに追従します。
- **`[シェル構成...]`**、**`[プロセスIDをリセット]`**、および **`[終了履歴を消去]`** は設定ダイアログ内に配置されています（ツールバーをすっきりと整理）。
- 設定・言語・テーマ・リスト/ターミナルの表示状態・レイアウトは `logs/config.json` に自動保存され、次回起動時に復元されます。

![設定ダイアログ](docs/images/settings_dialog.png)

### 7. タスクキュー: 順序・依存・タイムアウト・自動再実行
- ジョブを**1 コマンドずつ**登録すると、ProcessCommander が実行順・同時実行数・優先度を決め、キューを永続化します（`logs/taskqueues/<name>.json`）。
- ジョブ単位の `timeoutSec`、出力と CPU の両方を見るアイドル検出、成功条件（`exitCode` / `filesNewer` / `outputRegex`）、自動再実行（`maxAttempts`）、依存失敗時の `blocked` を提供します。
- **ChainGuard** が `start_process` / `taskqueue_add` / `taskqueue_update` のシェル連鎖と待ちループを拒否します。順序は `dependsOn` で宣言してください。CLI の `--allow-chain` のみ例外（MCP には非公開）。
- GUI: 左端の表示切替ボタン、または **Ctrl+T** で **2行同時表示 (Two Rows)**、**プロセスのみ (Processes Only)**、**タスクのみ (Tasks Only)** を順次トグル切り替え。2行表示時は水平スプリッターバーをドラッグして各領域の高さを自在に調整できます。実行中のジョブは上位に表示され、失敗は低彩度の赤、タイムアウトは低彩度のアンバーで表示されます。
- 詳細は [タスクキュー](#タスクキュー-順序依存タイムアウト自動再実行) を参照。

---

## ビルド方法

Windows 10/11 (x64)、Visual Studio 2022 (C++17)、CMake 3.20 以降が必要です。

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

ビルド成果物:
- `build/Release/ProcessCommander.exe` — GUI ダッシュボード、Named Pipe IPC サーバー、MCP stdio サーバー（`conpty.dll` / `OpenConsole.exe` と同梱）
- `build/Release/pcder.exe` — コンソール CLI クライアント

---

## 実行ファイル: `ProcessCommander.exe` と `pcder.exe`

| 実行ファイル | 役割 | 主なコマンド |
| :--- | :--- | :--- |
| `ProcessCommander.exe` | GUI ダッシュボード、Named Pipe IPC サーバー、MCP stdio サーバー。すべての ConPTY プロセスを所有。 | `ProcessCommander.exe gui`<br>`ProcessCommander.exe mcp`<br>`ProcessCommander.exe help` |
| `pcder.exe` | ダッシュボードへ要求を送るコンソールクライアント。独自 GUI なし。 | `pcder.exe list`<br>`pcder.exe start -- cmd.exe`<br>`pcder.exe help --json` |

- **`pcder.exe`** を推奨します。ダッシュボードが未起動なら同じフォルダの `ProcessCommander.exe` を自動起動します。**両方の exe を同じフォルダに置いてください。**
- 旧形式 `ProcessCommander.exe <command>` も同じ処理に転送されます（互換用）。
- `pcder.exe help --json` が機械可読な正規ガイドです（`concepts`, `data_dir_option`, `effective_data_dir`, `commands`, `setup.mcp.auto_install`, `agent` など）。

---

## 使い方 1: CLI コマンドとして呼び出す

CLI コマンドは Named Pipe (`\\.\pipe\ProcessCommander_IPC_v1`) を通じて起動中の GUI ダッシュボードと連携します（GUI 未起動時は自動で起動します）。

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

---

## タスクキュー: 順序・依存・タイムアウト・自動再実行

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

---

## 使い方 2: Claude の MCP サーバーとして登録する

`.mcp.json`（Claude Code 用）または `claude_desktop_config.json` に以下を追加すると、Claude から専用ツールとして直接呼び出せます。

```json
{
  "mcpServers": {
    "process-commander": {
      "command": "C:/wsgit/ProcessCommander/build/Release/ProcessCommander.exe",
      "args": ["mcp"]
    }
  }
}
```

`pcder.exe` に設定を書かせることもできます（既存設定にマージ、冪等）:

```powershell
pcder.exe mcp install                              # .mcp.json (Claude Code)
pcder.exe mcp install --client opencode            # opencode.json
pcder.exe mcp install --data-dir .processcommander # プロジェクト内データも一緒に
pcder.exe mcp install --dry-run --json             # 書き込まずに確認
```

### 提供される MCP ツール

**プロセス操作**

| ツール | 説明 |
| :--- | :--- |
| `start_process` | 1 コマンドを起動（`cmd`, `name`, `mode` [`pty`\|`pipe`\|`console`], `pty`, `open_stdin`, `cwd`, `log_file`, `done_file`, `timeout_sec`, `idle_timeout_sec`, `estimate_end`, `wait`, `wait_ms`）。連鎖コマンドは拒否されます。 |
| `list_processes` | 管理プロセスの一覧（`id` 任意）。 |
| `get_process_output` | 出力を取得（`id`, `tail`, `wait_ms`）。 |
| `send_process_input` | stdin / ConPTY へ入力（`id`, `input`, `newline`, `wait_ms`）。 |
| `wait_process` | 終了待機（`id`, `wait_ms`）。 |
| `kill_process` | プロセスツリーを強制終了（`id`; `0` で全実行中）。タスクキュー由来の試行は killed として再実行判定されます。 |

**タスクキュー操作**

| ツール | 説明 |
| :--- | :--- |
| `taskqueue_add` | ジョブ登録（`taskqueue`, `jobs[]`, `concurrency?`, `notifyFile?`）。各ジョブに `id`, `command`, `summary`, `timeoutSec` が必要。 |
| `taskqueue_list` | ジョブ一覧（`taskqueue?`, `state?`）。 |
| `taskqueue_get` | ジョブ詳細・試行履歴・指示書（`id`, `includeInstructions?`, `rev?`）。 |
| `taskqueue_retry` | failed/blocked/cancelled を再実行（`id`, `resetAttempts?`, `refreshInstructions?`）。blocked の後続も再キュー。 |
| `taskqueue_update` | pending/failed/blocked のジョブを更新。 |
| `taskqueue_cancel` | ジョブをキャンセル（実行中は kill、再実行しない）。 |
| `taskqueue_pause` / `taskqueue_resume` | キューの新規開始を停止 / 再開。 |
| `taskqueue_wait` | キューの version 変化をロングポーリング（`taskqueue`, `sinceVersion`, `timeoutMs`）。 |
| `taskqueue_purge` | 終了済みジョブを削除（`taskqueue`, `states[]`）。 |
| `taskqueue_link` / `taskqueue_unlink` | 依存を 1 本追加 / 削除（`job`, `dependsOn`, `on` = `success`\|`done`）。 |
| `taskqueue_graph` | 依存グラフ（`ready` / `waiting_for` / `dependents`）。 |

---

（以下の help 本文は英語です）

## 付録: `ProcessCommander.exe --help` の出力

```text
ProcessCommander - Native Win32 Multi-LLM Process & ConPTY Manager + Live GUI Dashboard

USAGE:
  ProcessCommander.exe [gui] [--data-dir <path>]
      Launch the live Win32 Dashboard GUI and Named Pipe server.

  ProcessCommander.exe mcp [--data-dir <path>]
      Run as a Model Context Protocol (MCP) stdio server for Claude.
      Automatically connects to (or launches) the Win32 GUI Dashboard.

  ProcessCommander.exe --help
      Show this help.

OPTIONS (per launch):
  --data-dir <path>   Store logs, state, task queues and config in <path>.
                      Use a project-local directory to git-manage them. Each
                      data dir gets its own dashboard instance (separate pipe).
                      Equivalent environment variable: PROCESS_COMMANDER_LOG_DIR.
                      Consistency: pass the same --data-dir on every call, or embed
                      it once with `pcder.exe mcp install --data-dir <path>`.
                      Different data dirs are isolated dashboard instances.

WORKING MODEL (LLM orchestration):
  ProcessCommander is the execution layer for a supervising LLM.

    Supervisor LLM (Claude Code, opencode, ...)
      |  start_process   -> one long-running worker command
      |  taskqueue_add   -> ordered/dependent worker jobs
      v
    ProcessCommander
      |  owns processes, timeouts, logs, retries, state
      v
    Worker LLM / build command
      -> one job command; writes artifacts and logs

  Assumed structure:
    1. The supervisor plans and declares jobs. It does not run the work itself,
       does not chain commands, and does not fabricate success.
    2. Each unit of work is one job with a one-line summary, its own timeoutSec,
       and an optional success check (filesNewer / outputRegex). Steps or
       dependencies become separate jobs linked with dependsOn.
    3. Workers are ordinary processes and may themselves be LLM CLIs. Pass
       detailed instructions via instructionsFile/instructionsText; workers read
       {instructions} / PC_JOB_INSTRUCTIONS and write artifacts.
    4. The supervisor observes with taskqueue_wait / taskqueue_list /
       taskqueue_graph and reads logs; it never polls .done files.
    5. On failure, fix the cause and call taskqueue_retry. Blocked dependents
       resume automatically.
    6. Keep the supervisor context small: read artifacts, not raw transcripts.

CONSOLE CLIENT / CLI SETUP:
  Use pcder.exe for CLI commands (start, list, logs, send, wait, kill, kill-all, clear).
  It auto-starts the GUI dashboard and locates ProcessCommander.exe in its folder.
  Run `pcder.exe help` for human-readable usage, or `pcder.exe help --json` for
  machine-readable commands and setup instructions for LLMs.

COMPLETION NOTIFICATION / BACKGROUND TASKS:
  `pcder.exe start ... --wait --wait-ms 0 --json` keeps pcder.exe alive until the
  managed process exits. `--wait-ms 0` means unlimited wait. Exit code is 0 on
  completion, the child exit code on failure, and 124 if the wait times out.
  For an existing process: `pcder.exe wait --id <id> --wait-ms 0 --json`.
  ProcessCommander also writes a .done JSON file on exit.

MCP SERVER SETUP:
  Register this executable in .mcp.json (Claude Code) or claude_desktop_config.json:
    {
      "mcpServers": {
        "process-commander": {
          "command": "<absolute path to ProcessCommander.exe>",
          "args": ["mcp"]
        }
      }
    }
  Auto-register (recommended for LLM agents):
    pcder.exe mcp install                    (writes .mcp.json in the current directory)
    pcder.exe mcp install --client opencode  (writes/updates opencode.json)

  PRECONDITIONS (LLM agents):
    - Windows 10/11 x64, same user session as the dashboard; no admin required.
    - The dashboard auto-starts on first use and owns the Named Pipe server.
    - Logs, state and task queues live under logs/ (override: PROCESS_COMMANDER_LOG_DIR).

  MCP tools (19):
    Process (6):
      start_process, list_processes, get_process_output, send_process_input,
      wait_process, kill_process
    Task queue (13):
      taskqueue_add, taskqueue_list, taskqueue_get, taskqueue_retry,
      taskqueue_update, taskqueue_cancel, taskqueue_pause, taskqueue_resume,
      taskqueue_wait, taskqueue_purge, taskqueue_link, taskqueue_unlink,
      taskqueue_graph

  HOW TO USE (decision guide):
    - One command, e.g. a build script, CLI agent or shell -> start_process
    - Multiple steps, order, dependencies or retries      -> taskqueue_add + dependsOn
    - Never write &&, ||, a single &, PowerShell ';', newline chains or polling
      loops. ChainGuard rejects them: one job = one command with its own timeoutSec.
    - Required taskqueue job fields: id, command, summary, timeoutSec.
      Optional: cwd, env, dependsOn, idle, maxAttempts, retryDelaySec, success,
      killExclude, priority, instructionsFile / instructionsText.
    - Success is verified: exit code AND optional filesNewer / outputRegex.
    - States: waiting = queued and ready; pending = dependencies not satisfied;
      blocked = a dependency failed; running; succeeded; failed; cancelled.
    - Observe with taskqueue_wait / taskqueue_list / taskqueue_graph; after fixing
      the cause use taskqueue_retry (blocked dependents resume automatically).

  For the full machine-readable guide (agent.quick_start, decision, states):
    pcder.exe help --json
```

---

## 付録: `pcder.exe help` の出力

```text
pcder - ProcessCommander console client

USAGE:
  pcder [--json] <command> [options]

GLOBAL OPTIONS:
  --json, -j               Print machine-readable JSON instead of human-friendly text.
  -h, --help               Show this help.

DATA DIRECTORY (per invocation):
  --data-dir <path>        Store logs, state, task queues and config in <path> for
                           THIS pcder call. May be placed before or after the command.
                           Point it at a project folder to git-manage the data:
                             pcder.exe --data-dir .processcommander taskqueue add --file jobs.json
                             pcder.exe taskqueue list main --data-dir .processcommander
                           Each data dir gets its own dashboard instance, so different
                           projects can run at the same time.

  CONSISTENCY (one data dir per project):
    --data-dir is per invocation, not a global setting. To keep a project's
    logs/state/task queues consistent:
      1. Choose one project-local directory, e.g. .processcommander in the project root.
      2. Register the MCP server once with it:
           pcder.exe mcp install --data-dir .processcommander
         This writes "--data-dir .processcommander" into the project's MCP config,
         so every MCP tool call for the project uses the same directory.
      3. Pass the same --data-dir on every direct pcder call.
      4. Relative paths resolve against the caller's current directory. MCP clients
         normally start the server in the project root; if the cwd is uncertain,
         put an absolute path in the MCP config.
      5. Different data dirs are isolated (own dashboard instance, Named Pipe and
         mutex); they never share processes or queues.
      6. Verify: pcder.exe --data-dir <path> help --json prints
         "effective_data_dir", and the dashboard status bar shows the same path.

PCDER CONCEPTS:
  Process IDs : Numeric IDs assigned by the dashboard (#1, #2, ...). Use them with
                list --id, logs --id, send --id, wait --id and kill --id.
  Modes       : pipe    = anonymous pipes (default; non-interactive CLI agents)
                pty     = Win32 ConPTY (interactive TUI: opencode, claude)
                console = separate visible console window
  stdin       : pipe mode keeps stdin writable (pcder send) unless --close-stdin;
                close it for programs that wait for EOF.
  Logs        : logs/proc_<id>_<name>_<timestamp>.log (or --log-file).
  Done file   : atomic JSON on exit (logs/proc_<id>.done or --done-file) with
                state, exit_code, reason, log_file and done_file.
  cwd / env   : --cwd defaults to the caller's directory; PWD and INIT_CWD are
                set for the child and OLDPWD is removed.
  JSON output : --json / -j before or after the command prints one JSON object.
  Lifecycle   : start returns immediately unless --wait; use wait --id or the
                .done file to detect completion.

EXIT CODES:
  0    success / completed
  1    CLI or configuration error (also: process failure without --wait)
  124  wait timed out while the process is still running
       start --wait / wait return the managed process's exit code

ENVIRONMENT:
  PROCESS_COMMANDER_LOG_DIR   Override the logs directory (state, logs, queues).

EXAMPLES:
  pcder.exe list --json
  pcder.exe start --name build --cmd "cmd /c build.cmd" --timeout 1800 --wait --wait-ms 0 --json
  pcder.exe start --name tui --pty --cmd "opencode"
  pcder.exe logs --id 1 --tail 80
  pcder.exe send --id 1 --input "status"
  pcder.exe taskqueue add --file jobs.json --taskqueue main
  pcder.exe mcp install --client opencode

COMMANDS:
  list [--id <id>]
      List all managed processes and their live metrics.

  logs --id <id> [--tail <lines>] [--wait-ms <ms>] [--raw]
      Fetch process output logs (ANSI codes stripped by default unless --raw).

  start [options] -- <command...>
      Start a managed process (e.g. claude, opencode) under a Win32 Job Object.
      Options:
        --cmd "<command>"        Command line to execute
        --name <label>           Display name on dashboard (e.g. "opencode-worker")
        --mode <pty|pipe|console>
                                 pty     = Win32 ConPTY interactive session
                                 pipe    = Anonymous pipes (default; no ConPTY)
                                 console = Separate visible console window
        --pty                    Shortcut for --mode pty (enable ConPTY).
        --open-stdin             Pipe mode only (default): keep stdin writable for pcder send.
        --close-stdin            Pipe mode only: close stdin after launch so non-interactive
                                 programs (e.g. opencode run) receive EOF and can finish.
        --cwd <dir>              Working directory (default: the caller's current directory)
        --log-file <path>        Custom path for live output log
        --done-file <path>       Custom completion marker file created on exit
        --timeout <sec>          Max runtime timeout in seconds (0 = unlimited)
        --idle-timeout <sec>     Hang timeout if no output for N seconds (0 = unlimited)
        --eta <time>             Estimated end time shown in gray while running
                                 (e.g. "18:30" or "2026-10-22 18:30:00").
                                 If omitted, --timeout shows "remaining N seconds".
        --estimate-end <time>    Alias for --eta
        --wait                   Block until process exits (unlimited unless --wait-ms is set)
        --wait-ms <ms>           Max wait when --wait is set; initial output wait otherwise
                                 (default: 500; 0 = unlimited with --wait)

  send --id <id> --input "<text>" [--no-newline] [--wait-ms <ms>]
      Send input text to a running process's stdin or ConPTY session.

  wait --id <id> [--wait-ms <ms>]
      Wait until the specified process exits (or wait-ms elapses).

  kill --id <id>
      Forcefully kill the entire process tree (Job Object) for <id>.

  kill-all
      Forcefully kill all running managed process trees.

  clear
      Remove finished processes from the dashboard list.

  mcp install [--client claude|opencode] [--path <file>] [--name <name>] [--data-dir <path>] [--dry-run]
      Register ProcessCommander as an MCP server automatically. Writes or updates
      .mcp.json (Claude Code, default) or opencode.json (--client opencode).
      --data-dir embeds "--data-dir <path>" in the generated MCP server args so the
      project keeps its logs/state/task queues inside that directory.
      --json prints the machine-readable result; --dry-run previews without writing.

TASK QUEUE:
  taskqueue add --file <jobs.json> [--taskqueue <name>] [--concurrency <n>] [--notify-file <path>]
      Register jobs (array, or an object with "jobs"/"taskqueue"/"concurrency").
      Each job needs id, command, summary, timeoutSec; dependsOn replaces '&&'.

  taskqueue list [<name>] [--state <state>]
      List jobs with state, queue, summary and blockedBy.

  taskqueue get <id> [--rev <n>] [--no-instructions]
  taskqueue retry <id>
  taskqueue cancel <id>
  taskqueue pause|resume <name>
  taskqueue graph [<name>] [--root <id>]
  taskqueue wait <name> [--since <version>] [--timeout-ms <ms>]
  taskqueue purge <name> [--states succeeded,failed,cancelled]

  start/run ... --allow-chain
      Allow a chained command (&&, ||, &, ';') as a single process. Task-queue
      jobs never allow chains; use dependsOn instead.

COMPLETION / BACKGROUND TASKS:
  Use --wait to keep pcder.exe alive until the managed process exits. Add
  --wait-ms 0 for an unlimited wait (or a positive value for a maximum).
  The exit code is 0 on completion, the child exit code on failure, and 124
  if the wait times out.

    pcder.exe start --name task --wait --wait-ms 0 --json --cmd "opencode run ..."
    pcder.exe wait --id <id> --wait-ms 0 --json

  ProcessCommander also writes a .done JSON file on exit. Without --wait,
  start returns immediately; poll the .done file or call wait.

SETUP FOR LLM AGENTS:
  CLI:
    Use pcder.exe (recommended) or the legacy "ProcessCommander.exe <command>" form.
    Run "pcder.exe help --json" to retrieve this help as machine-readable JSON.
    pcder.exe auto-starts the GUI dashboard on first use if it is not running.

  MCP (stdio):
    Register ProcessCommander.exe in .mcp.json (Claude Code) or
    claude_desktop_config.json (Claude Desktop):
      {
        "mcpServers": {
          "process-commander": {
            "command": "<path-to>\\ProcessCommander.exe",
            "args": ["mcp"]
          }
        }
      }

    AUTO-REGISTER (for LLM agents; no manual JSON editing):
      pcder.exe mcp install                    # .mcp.json (Claude Code)
      pcder.exe mcp install --client opencode  # opencode.json
      pcder.exe mcp install --path <file> --json --dry-run

    ORCHESTRATION MODEL (how an LLM should use this):
      A supervising LLM uses ProcessCommander as its execution layer to run
      child-process worker LLMs/CLIs as managed jobs.

        Supervisor LLM  ->  ProcessCommander  ->  Worker LLM / build command
        plans + declares    timeouts, logs,        one job command;
        jobs                retries, state         writes artifacts

      - The supervisor keeps its context small: it plans and declares jobs; it
        does not run the work itself, chain commands, or fabricate success.
      - One job = one command. Use start_process for a single worker; use
        taskqueue_add with dependsOn for multiple steps or dependencies.
      - Each job needs a one-line summary and its own timeoutSec. Pass detailed
        instructions via instructionsFile/instructionsText; the worker reads
        {instructions} / PC_JOB_INSTRUCTIONS and writes artifacts.
      - Workers may be LLM CLIs (opencode run, claude -p, ...) or build scripts.
      - Verify success with exit code + filesNewer/outputRegex; observe with
        taskqueue_wait / taskqueue_list / taskqueue_graph and logs; retry with
        taskqueue_retry. Blocked dependents resume automatically.
      - Never poll .done files; use taskqueue_wait or wait_process.

    MCP tools (19):
      Process:    start_process, list_processes, get_process_output,
                  send_process_input, wait_process, kill_process
      Task queue: taskqueue_add, taskqueue_list, taskqueue_get, taskqueue_retry,
                  taskqueue_update, taskqueue_cancel, taskqueue_pause, taskqueue_resume,
                  taskqueue_wait, taskqueue_purge, taskqueue_link, taskqueue_unlink,
                  taskqueue_graph

NOTE:
  The GUI dashboard is provided by ProcessCommander.exe, which is auto-started
  on first use if it is not already running.
```
