# ProcessCommander

**[English](README.md) | [日本語 (Japanese)](README-ja.md)**

**ProcessCommander** は、Claude Code や各種自動化スクリプトから複数のサブプロセス・対話型 CLI エージェント（`opencode`, `claude`, PowerShell, cmd, Git Bash 等）を並列起動・監視・制御し、**状況をひと目で把握できる Native Win32 + Direct2D マルチターミナルダッシュボード GUI** を備えたプロセスマネージャです。

![ProcessCommander Dashboard](docs/images/dashboard.png)

---

## 主な特徴

### 1. 一覧性と操作性を両立した Native Win32 + Direct2D ダッシュボード GUI
- **上部 KPI サマリーカード (6枚)**:
  - 管理ターミナル総数、稼働中 (`ACTIVE RUNNING`)、入力待ち/アイドル (`WAITING / IDLE >= 10s`、出力もCPU消費もない場合)、正常完了 (`COMPLETED`)、失敗/強制終了 (`FAILED / KILLED`)、および全 Job Object の合計プロセス数・合計メモリ使用量をリアルタイム表示。
- **全プロセス状況リスト（新しい順・全カラムソート対応）**:
  - 新しく起動したプロセスほど**リストの一番上**に追加されます（デフォルト: `ID ▼` 降順）。
  - リスト上部の**任意のカラムヘッダ**（`ID`, `Agent / Name`, `Status`, `Start Time (起動時刻)`, `Uptime (稼働時間)`, `PID (Tree)`, `Memory`, `CPU Time`, `Idle`, `Latest Output Preview`, `Command Line`）をクリックすると、**昇順 (`▲`) / 降順 (`▼`)** で自在にソートを切り替えられます。
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
- 隣の **`[Config]`** ボタンをクリックするとシェル設定ダイアログが開き、**`cmd`** (`cmd.exe`)、**`powershell`** (`powershell.exe`)、**`bash`** (`Git Bash` / `WSL`) のデフォルト切り替え・起動コマンドの編集・その場での個別起動が可能です（設定は `logs/config.json` に永続化されます）。

![Shell Config Dialog](docs/images/config_dialog.png)

### 3. 終了履歴の消去・非表示切り替え & タスク詳細ダイアログ
- **`[終了履歴を消去 (Clear Ended)]`**: 完了・強制終了したプロセスの履歴を一括消去し、`logs/processes_state.json` を更新します。
- **`[終了履歴を非表示 (Hide Ended)]`**: 履歴データを消さずに、終了したプロセスをリスト・タブから非表示にして「実行中のプロセスのみ」を絞り込み表示します（再度クリックで再表示）。
- **右クリックメニュー & タスク詳細ダイアログ**:
  - リストの行またはタブを**右クリック**（または行をダブルクリック）すると、「詳細を表示 (Task Details & Command Line)」「コマンドラインをコピー」「ログファイルを開く」「プロセスツリーを強制終了」「この履歴を削除」を実行できます。

### 4. タスクトレイ格納・復元対応
- ツールバーの **`[トレイに格納 (Tray)]`** ボタン、最小化ボタン、またはウィンドウ右上の閉じるボタン（`[X]`）で、バックグラウンドで全プロセスと IPC/MCP 通信を維持したまま **タスクトレイ（通知領域）** に格納されます。
- タスクトレイアイコンの**左クリック / ダブルクリック**（または右クリックメニュー）でいつでも元の画面に拡大・復元できます。完全に終了する場合はトレイアイコンの右クリックメニューから「ProcessCommander を完全終了 (Exit)」を選択します。

### 5. リアルタイムログ保存・状態復元・完了検知 `.done` ファイル出力
- **プロセス別リアルタイムログ**: すべてのプロセスの出力は `logs/proc_<id>_<name>_<YYYYMMDD_HHMMSS>.log`（または `--log-file <path>`）にリアルタイム保存されます。
- **完了検知用 `.done` マーカーファイル**: プロセス終了時に `logs/proc_<id>.done`（または `--done-file <path>`）へ終了コード・稼働時間・ログパスを含む JSON をアトミックに出力します。Claude 等からファイルの存在チェックだけで確実に終了を検知できます。
- **再起動時の状態復元**: プロセス一覧と終了履歴は `logs/processes_state.json` に自動保存され、次回起動時に復元されます。

---

## ビルド方法

Windows 10/11 (x64)、Visual Studio 2022 (C++17)、CMake 3.20 以降が必要です。

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

ビルド成果物: `build/Release/ProcessCommander.exe`

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

### 提供される MCP ツール
| ツール名 | 説明 |
| :--- | :--- |
| `start_process` | プロセス起動（`cmd`, `name`, `mode` [`pty`\|`pipe`\|`console`, 既定 `pipe`], `pty`, `open_stdin`, `cwd`, `log_file`, `done_file`, `timeout_sec`, `idle_timeout_sec`, `wait`, `wait_ms`） |
| `list_processes` | 全管理プロセスのステータス・起動時刻・稼働時間・PIDツリー数・メモリ・ログ/doneパス・最新出力行の一覧取得 |
| `get_process_output` | 指定プロセスの出力ログ取得（`id`, `tail_lines`, `clean_ansi`） |
| `send_process_input` | 実行中プロセスの ConPTY / stdin へテキスト入力送信（`id`, `input`, `append_newline`） |
| `wait_process` | 指定プロセスの終了待機（`id`, `wait_ms`） |
| `kill_process` | 指定プロセスツリー（`id`）または全実行中プロセス（`kill_all: true`）の強制終了 |
| `focus_process_window` | 指定プロセスのターミナルタブまたはコンソールウィンドウを前面表示（`id`） |
