# ProcessCommander

## Build

### MSVC ランタイムは静的リンク
- `CMakeLists.txt` で `CMAKE_MSVC_RUNTIME_LIBRARY` を `MultiThreaded`（Debug は `MultiThreadedDebug`）に設定し、VC++ ランタイムを静的リンクしている（`/MT`）。
- そのため `ProcessCommander.exe` / `pcder.exe` は VC++ 再頒布可能パッケージなしで動く。インストーラー（`installer/ProcessCommander.iss`）に `vcruntime140.dll` などを同梱する必要はない。
- リンクする静的ライブラリもすべて `/MT` でビルドすること。`/MD` のライブラリを混ぜると `__imp_strerror` などの未解決シンボルでリンクエラーになる。

### lib/libsixel.lib の作り直し
- `lib/libsixel.lib` は `src/libsixel-windows` から `/MT` でビルドしたもの（同ディレクトリの `CMakeLists.txt` で静的ランタイムを指定済み）。
- 作り直す手順: `src\libsixel-windows\build.bat` を実行し、`src\libsixel-windows\build\Release\libsixel.lib` を `lib\libsixel.lib` にコピーする。
- Release 版しかないため、Debug ビルドでは `LNK4098 (LIBCMT)` の警告が出るが無害。

### バージョン
- バージョンは `CMakeLists.txt` の `project(... VERSION x.y.z)` だけで管理する。C++ の `PC_VERSION`、exe の VERSIONINFO（`src/version_info.rc2`）、インストーラー（`installer/build_installer.cmd` が `/DMyAppVersion` で渡す）はすべてここから決まる。
