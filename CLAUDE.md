# cdreader 開発ルール

## Issue と Pull Request
- 要求・機能は GitHub issue で管理する。新しい要求は個別の issue として立てる。
- PR の説明には、対応する issue を必ず記載する (`.github/pull_request_template.md` の「関連 Issue」)。
  - その PR で完了する issue: `Closes #番号`
  - 一部対応・関連のみ: `Refs #番号`
- issue にない作業を始めるときは、先に issue を作る。

## ビルドとテスト
- C++17 / CMake。`core/` はプラットフォーム非依存 (Windows / Android で共有) に保つ。
- プラットフォーム固有のコードは `platform/<os>/` に置く。
- PR 前にテストを実行する: `cmake -S . -B build && cmake --build build && ctest --test-dir build`
- README は日本語で書く。Claude Code を利用している旨の記載を残す。
