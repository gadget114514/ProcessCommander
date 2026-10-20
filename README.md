# ProcessCommander

Claude から他の LLM プログラム（`opencode`, `claude` 等）やサブプロセスを起動・監視・対話制御し、**状況をひと目で把握できる Native Win32 ダッシュボード GUI** を備えたプロセスマネージャです。

---

## 主な特徴

1. **一覧性重視の Native Win32 ダッシュボード GUI**
   - **上部 KPI サマリーカード (6枚)**: 管理プロセス総数、稼働中 (Active)、待機/無応答注意 (Idle >= 10s)、正常完了、失敗/強制終了、および全 Job Object の合計プロセス数・合計メモリ使用量をリアルタイム表示。
   - **全プロセス状況テーブル (色分け表示)**: ID、エージェント名、ステータスバッジ、実行モード (`pty`/`pipe`/`console`)、ルートPID＋Job内プロセス数、メモリ使用量、経過時間/制限時間、無通信時間 (Idle)、**最新出力1行プレビュー** を一覧表示。
   - **左下 2x2 マルチエージェント・ライブタイルモニター**: 最大4つの並列プロセス（`opencode` や `claude` 等）の最新ターミナル出力（`TerminalBuffer` から抽出）を同時にリアルタイム監視可能。タイルをクリックすると右側の Direct2D `TerminalView` が即座に切り替わります。
   - **右下 擬似PTYターミナル (`TerminalView`) & 対話入力バー**: `src/Terminal` および `include` の Direct2D / DirectWrite / IME 対応フルVTターミナルエミュレータ（`TerminalView` + `TerminalEmulator` + `TerminalBuffer` + `ConPtySession`）を埋め込み表示。直接ターミナル内をクリックしてキー入力・IME変換・マウス操作・スクロールバックができるほか、下部の入力バーからの送信 (`Send Input`)、`Ctrl+C` 送信、およびツールバーの **`[Grid View]`** ボタンによる DWM サムネイル一覧オーバーレイ（`TerminalGridView`）にも対応。

2. **3つの実行モード (`--mode`)**
   - `pty` (**Win32 ConPTY / 擬似コンソール**): `CreatePseudoConsole` を使用し、TUI や対話プロンプトを持つ `opencode` や `claude` の対話セッションを維持。ANSI エスケープシーケンスの自動除去機能付き。
   - `pipe` (**匿名パイプ**): `claude -p "..."` や `opencode run "..."` 等の非対話・バッチ実行向け。
   - `console` (**別コンソールウィンドウ表示**): `CREATE_NEW_CONSOLE` で独立したコンソール画面を開き、人間が直接画面を見たり操作したりしつつ、Job Object でツリー監視・強制終了が可能。
   - ※ Windows 上で `.cmd` / `.bat` としてインストールされている `claude` や `opencode` も `SearchPathW` により自動解決して起動します。

3. **確実なプロセスツリー管理 (Win32 Job Object)**
   - 各プロセスに専用の `Job Object` (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`) を割り当て、子・孫プロセス（Node.js、Python、サブシェル等）の生存数とメモリ使用量を追跡し、`kill` やタイムアウト時にはツリー全体を確実に強制終了します。
   - **全体タイムアウト (`--timeout`)** と **無応答ハングタイムアウト (`--idle-timeout`)** の両方に対応。

4. **Claude からの2通りの呼び出し方式（CLI コマンド & MCP サーバー両対応）**
   - どちらの方式で呼び出しても、裏で Named Pipe (`\\.\pipe\ProcessCommander_IPC_v1`) を通じて GUI ダッシュボード本体と連携します（GUI 未起動時は自動でダッシュボードが立ち上がります）。

---

## ビルド方法

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

ビルド成果物: `build/Release/ProcessCommander.exe`

---

## 使い方 1: Claude から CLI コマンドとして呼び出す

### プロセスの起動 (`start`)
```powershell
# 非対話モード (pipe) で claude を起動
ProcessCommander.exe start --name "claude-review" --mode pipe --timeout 120 --cmd "claude -p \"Review src/main.cpp\""

# 対話モード (ConPTY) で opencode を起動
ProcessCommander.exe start --name "opencode-agent" --mode pty --cmd "opencode"

# 別コンソールウィンドウ (console) を開いて起動
ProcessCommander.exe start --name "visible-agent" --mode console --cmd "claude"
```

### 稼働状況の一覧取得 (`list`)
```powershell
ProcessCommander.exe list
```

### 出力ログの取得 (`logs`)
```powershell
ProcessCommander.exe logs --id 1 --tail 50
```

### 対話セッションへの入力送信 (`send`)
```powershell
ProcessCommander.exe send --id 1 --input "yes"
```

### 完了待機 (`wait`) / プロセスツリー強制終了 (`kill`)
```powershell
ProcessCommander.exe wait --id 1 --wait-ms 10000
ProcessCommander.exe kill --id 1
ProcessCommander.exe kill-all
```

---

## 使い方 2: Claude の MCP サーバーとして登録する

`claude_desktop_config.json` または `.mcp.json`（Claude Code 用）に以下を追加すると、Claude が専用ツールとして `ProcessCommander` を直接呼び出せます。

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
- `start_process`: プロセス起動（`cmd`, `name`, `mode` [`pipe`|`pty`|`console`], `cwd`, `timeout_sec`, `idle_timeout_sec`, `wait`, `wait_ms`）
- `list_processes`: 全管理プロセスのステータス・メモリ・Job内プロセス数・最新出力行の一覧取得
- `get_process_output`: 指定プロセスの出力ログ取得（ANSI 除去済みテキスト）
- `send_process_input`: 実行中プロセスの stdin / ConPTY へテキスト入力送信
- `kill_process`: 指定 ID（または全プロセス）の Job Object ツリー強制終了
- `wait_process`: 指定プロセスの終了待機
