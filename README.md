# cdreader

音楽 CD (CD-DA) をリッピングして WAV ファイルに保存するツールです。
対象環境は **Windows** と **Android** で、まずは Windows 版 (コマンドライン) から実装しています。

> **このプロジェクトは [Claude Code](https://claude.com/claude-code) (Anthropic の AI コーディングエージェント) を利用して開発しています。**
> 設計・コード・テスト・ドキュメントの多くは Claude Code によって生成され、人がレビューしています。

## 特徴

- SCSI/MMC コマンド (`READ TOC`, `READ CD`) でドライブから直接オーディオセクタを読み取り
- 44.1 kHz / 16 bit / ステレオの WAV で保存 (トラックごとに `TrackNN.wav`、または `--single-file` で 1 ファイルのイメージ)
- WAV へのタグ書き込み (RIFF `LIST`/`INFO` と ID3v2.4 の `id3 ` チャンク、UTF-8)
- CUE シートの出力 (シングルファイル / トラックごとのどちらでも)
- 読み取りエラー時のリトライ、失敗したブロックはセクタ単位で再読込し、読めないセクタだけを無音で補完
- `--verify` で 2 回読みして一致を確認 (セキュアモード)
- ドライブの読み取りオフセット補正 (`--offset`、EAC / AccurateRip と同じ値)
- トラックごとの CRC32 と `rip.log` (TOC・結果) を出力
- CD-Extra (エンハンスド CD) のデータトラック・セッション間ギャップを考慮
- freedb (CDDB) ディスク ID の計算

## 使い方 (Windows)

```
cdreader drives                     光学ドライブの一覧
cdreader toc D:                     TOC (トラック一覧) を表示
cdreader rip D:                     全オーディオトラックを cd_<CDDB ID>\ に保存
cdreader rip D: -t 1,3-5 -o out     トラック 1,3,4,5 を out\ に保存
cdreader rip D: --verify -r 10      2 回読み比較、リトライ 10 回
cdreader rip D: --offset 6          読み取りオフセット +6 サンプルで補正
cdreader rip D: --single-file       全トラックを 1 つの WAV と CUE シートに保存
```

| オプション | 説明 |
| --- | --- |
| `-o, --output <dir>` | 出力先ディレクトリ (既定: `cd_<CDDB ID>`) |
| `-t, --tracks <list>` | リッピングするトラック (例: `1,3-5`)。既定は全オーディオトラック |
| `-r, --retries <n>` | 読み取り失敗時のリトライ回数 (既定: 5) |
| `--verify` | 全ブロックを 2 回読みして比較 (低速) |
| `--offset <n>` | 読み取りオフセット補正 (サンプル単位、負の値も可。既定: 0) |
| `--single-file` | 選択したトラックを 1 つのファイルにつなげて保存 (ディスクイメージ + CUE シート) |

終了コード: `0` 成功 / `1` エラー / `2` 読めないセクタがあった。

### 読み取りオフセット補正

CD ドライブは機種ごとに、要求した位置から一定サンプル数ずれた音声データを返します (読み取りオフセット)。
補正しないとトラックの境界が数サンプル〜数百サンプルずれ、AccurateRip などで照合できません。

- 値は [AccurateRip のドライブオフセット一覧](https://www.accuraterip.com/driveoffsets.htm) で
  `cdreader drives` に表示される機種名を探し、"Correction Offset" の値を `--offset` に指定します
  (EAC の「読み取りオフセット補正値」と同じ符号です。例: `+6`, `+667`, `-472`)。
- 1 サンプル = 4 バイト (16 bit ステレオ)。`+N` の場合、各トラックはドライブが返すデータの N サンプル後ろから切り出されます。
- 補正によってディスクの先頭より前・リードアウトより後 (CD-Extra ではデータセッションとの間) にはみ出した部分は、
  多くのドライブで読めないため無音で埋めます。その数は `rip.log` に記録されます。

### タグ

トラックのメタデータ (タイトル・アーティスト・アルバム・トラック番号・年・ジャンル・CDDB ディスク ID) を WAV に書き込みます。
現状は CDDB 照会 ([#6](https://github.com/noribow/cdreader/issues/6)) が未実装のため、書き込まれるのはトラック番号とディスク ID だけです。

- RIFF `LIST`/`INFO` チャンク: `INAM` タイトル、`IART` アーティスト、`IPRD` アルバム、`ITRK` トラック番号、`ICRD` 年、`IGNR` ジャンル、`ICMT` CDDB ディスク ID。
- `id3 ` チャンク (ID3v2.4、UTF-8): `TIT2` `TPE1` `TALB` `TPE2` `TRCK` (`番号/総数`) `TDRC` `TCON` `TXXX:DISCID`。
  INFO を読まずに ID3 だけを読むプレイヤーが多いため両方書きます。
- タグはオーディオデータ (`data` チャンク) の **後ろ** に置きます。ヘッダーは従来どおり 44 バイトの標準形のままなので、
  「サンプルは 44 バイト目から」と決め打ちする単純なツールでも読めます。RIFF ではチャンクの順序は自由で、
  ffmpeg / MediaInfo / ExifTool などは後ろのチャンクも読みます。
- INFO には文字コードの規定がありません。UTF-8 で書いており ffmpeg・MediaInfo は正しく表示しますが、
  ANSI コードページとして読むソフト (ExifTool の既定設定など) では日本語が化けます。ID3 側は UTF-8 が明示されています。

### CUE シート

リッピングのたびに、出力先に `<アーティスト> - <アルバム>.cue` (アルバム名が不明なら `CDImage.cue`) を書き出します。

- `--single-file`: 選択したトラックを 1 つのファイル (`<アーティスト> - <アルバム>.wav` / `CDImage.wav`) につなげて保存し、
  CUE シートの `INDEX 01` でファイル先頭からの各トラックの位置 (`mm:ss:ff`、ff = 1/75 秒) を示します。
  ディスク上で連続したトラックだけを指定できます (例: `-t 3-7`。`-t 1,3` はエラー)。
  オフセット補正をしても各トラックは同じだけずれた位置から切り出されるため、トラック同士は隙間・重複なくつながります
  (イメージの内容 = トラックごとにリッピングした結果を連結したもの。テストで確認しています)。
- トラックごとのリッピングでは、トラックごとに `FILE` 行を持つ CUE シートになります。
- `TITLE` / `PERFORMER`、`REM GENRE` / `REM DATE` / `REM DISCID`、TOC のコントロールビットから `FLAGS DCP` (デジタルコピー許可) / `FLAGS PRE` (プリエンファシス) を書きます。
  `FILE` の種類は MP3 / AIFF 以外はすべて `WAVE` (EAC・foobar2000 と同じく、FLAC などでも `WAVE`)。
- CUE には文字列のエスケープがないため、`"` は `'` に、改行などの制御文字は空白に置き換えます。改行コードは CRLF です。
- 文字コードは UTF-8 です。ASCII だけの場合は BOM なし、日本語などを含む場合は BOM 付きにします。
  ANSI コードページ (日本語 Windows では Shift_JIS) を既定とするソフトは BOM がないと UTF-8 と判別できない一方、
  古いパーサー (cuetools 1.4 / libcue など) は BOM を 1 行目の一部として扱いその行を読み飛ばします。
  そのため 1 行目は常に読み飛ばされても困らない `REM COMMENT "cdreader"` にしています。
- プリギャップ (`INDEX 00`) は現状読み取っていません。トラック間のギャップは前のトラックの末尾に含まれます (EAC の「ギャップを前のトラックに付加」と同じ)。
  トラック 1 の前の隠しトラック (HTOA) も保存しません。

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

テストは仮想ドライブ (`tests/fake_drive.*`) を使い、TOC 解析・リトライ・不良セクタ処理・2 回読み比較・オフセット補正・WAV 出力・
タグ・CUE シート・Ogg ページを検証します。
環境変数 `CDREADER_TEST_OUTPUT` にディレクトリを指定すると、テストで作ったサンプル (タグ付き WAV、CUE、Ogg) をそこに残すので、
ffprobe / MediaInfo / ExifTool / ogginfo などの外部ツールで確認できます。

## 構成

```
core/       プラットフォーム非依存のコア (Windows / Android で共有)
  scsi      ScsiTransport インターフェース、センスデータ解析
  cd_drive  MMC コマンド (INQUIRY, TEST UNIT READY, READ TOC, READ CD)
  toc       TOC 解析、CDDB ID
  ripper    リトライ・セクタ分割・verify を含むトラック読み取り
  audio_writer  出力フォーマットの共通インターフェース (WAV など。コーデック・コンテナはここに追加)
  metadata  アルバム / トラック情報 (タグ付け・ファイル名用)
  tags      RIFF INFO / ID3v2.4 タグの生成
  cue_sheet CUE シートの生成
  file_name ファイル名に使えない文字の置き換え
  ogg       Ogg コンテナ (ページ分割・CRC。Ogg Opus / Ogg FLAC 用、コーデック非依存)
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
- [ ] CDDB 対応 (ディスク情報の取得) — [#6](https://github.com/noribow/cdreader/issues/6)
- [ ] オーディオコーデック対応 (FLAC など) — [#7](https://github.com/noribow/cdreader/issues/7)
- [ ] コンテナフォーマット対応・タグ付け — [#8](https://github.com/noribow/cdreader/issues/8)
  - [x] WAV のタグ (INFO / ID3)、シングルファイル + CUE シート、Ogg ページ書き出し (コア)
  - [ ] Ogg Opus (Opus エンコーダ待ち)、Ogg FLAC
  - [ ] M4A (AAC / ALAC)、MKA (Matroska)
  - [ ] プリギャップ (`INDEX 00`) と HTOA の検出 (サブチャンネル Q の読み取り)
- [ ] セキュアモードでのドライブキャッシュ回避 (現状の `--verify` はキャッシュされたデータを再読込する可能性があります)
- [ ] Windows GUI

## 開発の進め方

要求・機能は [GitHub issue](https://github.com/noribow/cdreader/issues) で管理し、PR には対応する issue を記載します (`Closes #番号`)。
詳しくは [CLAUDE.md](CLAUDE.md) を参照してください。

## ライセンス

[BSD 2-Clause License](LICENSE)
