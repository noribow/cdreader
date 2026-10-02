# cdreader

音楽 CD (CD-DA) をリッピングして WAV ファイルに保存するツールです。
対象環境は **Windows** と **Android** で、まずは Windows 版 (コマンドライン) から実装しています。

> **このプロジェクトは [Claude Code](https://claude.com/claude-code) (Anthropic の AI コーディングエージェント) を利用して開発しています。**
> 設計・コード・テスト・ドキュメントの多くは Claude Code によって生成され、人がレビューしています。

## 特徴

- SCSI/MMC コマンド (`READ TOC`, `READ CD`) でドライブから直接オーディオセクタを読み取り
- 44.1 kHz / 16 bit / ステレオの WAV で保存 (トラックごとに `NN - 曲名.wav`、曲名が不明なら `TrackNN.wav`)
- 読み取りエラー時のリトライ、失敗したブロックはセクタ単位で再読込し、読めないセクタだけを無音で補完
- `--verify` で 2 回読みして一致を確認 (セキュアモード)
- ドライブの読み取りオフセット補正 (`--offset`、EAC / AccurateRip と同じ値)
- トラックごとの CRC32 と `rip.log` (TOC・結果) を出力
- CD-Extra (エンハンスド CD) のデータトラック・セッション間ギャップを考慮
- CDDB (既定は [gnudb.org](https://gnudb.org/)) からアルバム名・アーティスト・曲名などを取得し、
  フォルダ名・ファイル名・`rip.log` に使用

## 使い方 (Windows)

```
cdreader drives                     光学ドライブの一覧
cdreader toc D:                     TOC (トラック一覧) と CDDB のディスク情報を表示
cdreader rip D:                     全オーディオトラックを "アーティスト - アルバム"\ に保存
cdreader rip D: -t 1,3-5 -o out     トラック 1,3,4,5 を out\ に保存
cdreader rip D: --verify -r 10      2 回読み比較、リトライ 10 回
cdreader rip D: --offset 6          読み取りオフセット +6 サンプルで補正
cdreader rip D: --cddb-match 2      CDDB の候補が複数あるとき 2 番目を使う
cdreader rip D: --no-cddb           CDDB に問い合わせない (cd_<CDDB ID>\TrackNN.wav)
```

| オプション | 説明 |
| --- | --- |
| `-o, --output <dir>` | 出力先ディレクトリ (既定: `アーティスト - アルバム`。CDDB で見つからなければ `cd_<CDDB ID>`) |
| `-t, --tracks <list>` | リッピングするトラック (例: `1,3-5`)。既定は全オーディオトラック |
| `-r, --retries <n>` | 読み取り失敗時のリトライ回数 (既定: 5) |
| `--verify` | 全ブロックを 2 回読みして比較 (低速) |
| `--offset <n>` | 読み取りオフセット補正 (サンプル単位、負の値も可。既定: 0) |

CDDB のオプション (`rip` と `toc` の両方で使えます):

| オプション | 説明 |
| --- | --- |
| `--no-cddb` | CDDB に問い合わせない |
| `--cddb-server <url>` | CDDB サーバー (HTTP の CGI の URL。既定: `https://gnudb.gnudb.org/~cddb/cddb.cgi`) |
| `--cddb-match <n>` | 候補が複数あるときに使う候補の番号 (1 から。既定: 1) |
| `--cddb-hello <user@host>` | CDDB の hello に送るユーザー名とホスト名 (既定: `cdreader@localhost`。実際のユーザー名は送りません) |

終了コード: `0` 成功 / `1` エラー / `2` 読めないセクタがあった。

### CDDB とファイル名

`rip` / `toc` は TOC から freedb 形式のディスク ID を計算し、CDDB サーバーに `cddb query` → `cddb read` (HTTP, proto 6 = UTF-8) で問い合わせます。

- 候補が複数ある場合 (完全一致が複数、または近似一致) は 1 番目を使い、全候補を画面と `rip.log` に表示します。
  別の候補を使うには `--cddb-match <n>` を指定します (`cdreader toc D:` で候補を確認できます)。
- 取得したアルバム名・アーティスト・年・ジャンル・曲名は `rip.log` に記録され、出力ファイルのメタデータとして渡されます
  (タグの書き込みは出力フォーマット側の対応次第です。WAV には現在タグを書きません)。
- コンピレーション (`Various Artists` など、曲名が `アーティスト / 曲名` 形式) は曲ごとのアーティストに分けます。
- UTF-8 として不正なデータ (古い Latin-1 のエントリ) は Latin-1 として読み替えます (Shift_JIS など他の文字コードで登録されたエントリは文字化けします)。
- 通信エラーや該当なしでもリッピングは中断せず、従来どおり `cd_<CDDB ID>\TrackNN.wav` に保存します。

ファイル名・フォルダ名は Windows / Android で使えない文字 (`< > : " / \ | ? *` と制御文字) を `_` に置き換え、
末尾のピリオド・空白を取り除き、`CON` や `NUL` などの予約名には先頭に `_` を付けます。日本語などはそのまま使います。

| 曲名 / アルバム情報 | 出力 |
| --- | --- |
| あり | `アーティスト - アルバム\01 - 曲名.wav` |
| なし (見つからない・`--no-cddb`) | `cd_<CDDB ID>\Track01.wav` |

### 読み取りオフセット補正

CD ドライブは機種ごとに、要求した位置から一定サンプル数ずれた音声データを返します (読み取りオフセット)。
補正しないとトラックの境界が数サンプル〜数百サンプルずれ、AccurateRip などで照合できません。

- 値は [AccurateRip のドライブオフセット一覧](https://www.accuraterip.com/driveoffsets.htm) で
  `cdreader drives` に表示される機種名を探し、"Correction Offset" の値を `--offset` に指定します
  (EAC の「読み取りオフセット補正値」と同じ符号です。例: `+6`, `+667`, `-472`)。
- 1 サンプル = 4 バイト (16 bit ステレオ)。`+N` の場合、各トラックはドライブが返すデータの N サンプル後ろから切り出されます。
- 補正によってディスクの先頭より前・リードアウトより後 (CD-Extra ではデータセッションとの間) にはみ出した部分は、
  多くのドライブで読めないため無音で埋めます。その数は `rip.log` に記録されます。

ドライブへのアクセスは SCSI パススルー (`IOCTL_SCSI_PASS_THROUGH_DIRECT`) を使います。
"Access is denied" になる環境では管理者として実行してください。

## ビルド

CMake 3.16 以上と C++17 コンパイラが必要です。

### Visual Studio (MSVC)

```
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

`build\Release\cdreader.exe` が生成されます (ランタイム静的リンクのため単体で動作)。

### MinGW-w64 (Linux からのクロスビルドも可)

```
cmake -S . -B build-win -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix
cmake --build build-win
```

### コアライブラリのテストのみ (Linux / macOS)

```
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

テストは仮想ドライブ (`tests/fake_drive.*`) を使い、TOC 解析・リトライ・不良セクタ処理・2 回読み比較・オフセット補正・WAV 出力を検証します。
CDDB は偽の `HttpClient` を使い (ネットワークには接続しません)、問い合わせコマンドの生成・応答コード・xmcd エントリの解析・ファイル名の変換を検証します。

## 構成

```
core/       プラットフォーム非依存のコア (Windows / Android で共有)
  scsi      ScsiTransport インターフェース、センスデータ解析
  cd_drive  MMC コマンド (INQUIRY, TEST UNIT READY, READ TOC, READ CD)
  toc       TOC 解析、CDDB ID
  cddb      CDDB の問い合わせ・応答解析 (HttpClient 経由)
  file_naming  メタデータからのファイル名・フォルダ名 (使えない文字の置換)
  ripper    リトライ・セクタ分割・verify を含むトラック読み取り
  audio_writer  出力フォーマットの共通インターフェース (WAV など。コーデック・コンテナはここに追加)
  metadata  アルバム / トラック情報 (タグ付け・ファイル名用)
  http      オンライン照会用 HTTP インターフェース (実装はプラットフォーム側)
  wav_writer, crc32
platform/windows/   SPTI による ScsiTransport 実装、ドライブ列挙、WinHTTP クライアント
app/cli/            Windows 用コマンドラインツール
tests/              仮想ドライブを使ったユニットテスト
```

プラットフォーム固有なのは「SCSI コマンドをドライブに送る」部分 (`ScsiTransport`) だけです。
Android 版では USB ホスト API 経由の USB Mass Storage (Bulk-Only Transport) で同じ MMC コマンドを送る
`ScsiTransport` を実装し、コアを NDK で共有する予定です。

## 今後の予定

- [ ] Android 版 (USB 外付け CD ドライブ、NDK + Kotlin UI) — [#9](https://github.com/noribow/cdreader/issues/9)
- [ ] オフセット値の自動検出 (AccurateRip データベースとの照合)、リードイン/リードアウトのオーバーリード
- [ ] AccurateRip 対応 — [#5](https://github.com/noribow/cdreader/issues/5)
- [x] CDDB 対応 (ディスク情報の取得) — [#6](https://github.com/noribow/cdreader/issues/6)
- [ ] MusicBrainz 対応、CD-TEXT の読み取り (CDDB に無いディスクの情報取得)
- [ ] オーディオコーデック対応 (FLAC など) — [#7](https://github.com/noribow/cdreader/issues/7)
- [ ] コンテナフォーマット対応・タグ付け — [#8](https://github.com/noribow/cdreader/issues/8)
- [ ] セキュアモードでのドライブキャッシュ回避 (現状の `--verify` はキャッシュされたデータを再読込する可能性があります)
- [ ] Windows GUI

## 開発の進め方

要求・機能は [GitHub issue](https://github.com/noribow/cdreader/issues) で管理し、PR には対応する issue を記載します (`Closes #番号`)。
詳しくは [CLAUDE.md](CLAUDE.md) を参照してください。

## ライセンス

[BSD 2-Clause License](LICENSE)
