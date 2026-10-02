# cdreader

音楽 CD (CD-DA) をリッピングして WAV ファイルに保存するツールです。
対象環境は **Windows** と **Android** で、まずは Windows 版 (コマンドライン) から実装しています。

> **このプロジェクトは [Claude Code](https://claude.com/claude-code) (Anthropic の AI コーディングエージェント) を利用して開発しています。**
> 設計・コード・テスト・ドキュメントの多くは Claude Code によって生成され、人がレビューしています。

## 特徴

- SCSI/MMC コマンド (`READ TOC`, `READ CD`) でドライブから直接オーディオセクタを読み取り
- 44.1 kHz / 16 bit / ステレオの WAV で保存 (トラックごとに `TrackNN.wav`)
- 読み取りエラー時のリトライ、失敗したブロックはセクタ単位で再読込し、読めないセクタだけを無音で補完
- `--verify` で 2 回読みして一致を確認 (セキュアモード)
- ドライブの読み取りオフセット補正 (`--offset`、EAC / AccurateRip と同じ値)
- トラックごとの CRC32 と `rip.log` (TOC・結果) を出力
- [AccurateRip](https://www.accuraterip.com/) データベースとの照合 (チェックサム v1 / v2、confidence を表示)
- AccurateRip を使ったドライブの読み取りオフセットの自動検出 (`cdreader offset`)
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
cdreader rip D: --no-accuraterip    AccurateRip の照合 (ネットワークアクセス) をしない
cdreader offset D:                  読み取りオフセットを AccurateRip で検出
```

| オプション | 説明 |
| --- | --- |
| `-o, --output <dir>` | 出力先ディレクトリ (既定: `cd_<CDDB ID>`) |
| `-t, --tracks <list>` | リッピングするトラック (例: `1,3-5`)。既定は全オーディオトラック |
| `-r, --retries <n>` | 読み取り失敗時のリトライ回数 (既定: 5) |
| `--verify` | 全ブロックを 2 回読みして比較 (低速) |
| `--offset <n>` | 読み取りオフセット補正 (サンプル単位、負の値も可。既定: 0) |
| `--no-accuraterip` | リッピング後に AccurateRip データベースを照会しない (チェックサムは `rip.log` に記録されます) |

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

### AccurateRip による照合

リッピングが終わると、AccurateRip データベース (`http://www.accuraterip.com/accuraterip/.../dBAR-<トラック数>-<ID1>-<ID2>-<CDDB ID>.bin`)
を取得し、各トラックのチェックサムを世界中のほかのユーザーのリッピング結果と照合します。結果はコンソールと `rip.log` に出力されます。

```
AccurateRip (disc id 011-0010e21a-00b8b1b7-950bd70b)
Track 01  v1 1A2B3C4D  v2 5E6F7081  Accurately ripped (confidence 12/15, v2)
Track 02  v1 ...       v2 ...       Not accurate (confidence 0/14)
Track 03  v1 ...       v2 ...       Not in database
AccurateRip: 1 of 3 track(s) accurately ripped (2 pressing(s) in database)
```

| 表示 | 意味 |
| --- | --- |
| `Accurately ripped (confidence N/M, v1/v2)` | データベースのチェックサムと一致。N はこの結果と一致した登録数、M はこのトラックの登録数の合計。一致したチェックサムの種類 (v1 / v2) も表示 |
| `Not accurate (confidence 0/M)` | 登録はあるが一致しない。読み取りエラー、読み取りオフセットの誤り、別プレス (別の盤) の可能性 |
| `Not in database` | このトラックの登録がない |

- ディスクの識別にはオーディオトラックの開始位置とリードアウト位置から計算した AccurateRip ディスク ID を使います。
  CD-Extra (エンハンスド CD) ではデータトラックを除いたオーディオトラックと、ディスク全体 (データセッション込み) のリードアウトを使います (dBpoweramp / EAC と同じ)。
- チェックサムはオフセット補正後のデータで計算し、ディスク先頭のトラックの最初の 5 セクタ (正確には 2939 サンプル) と最後のトラックの最後の 5 セクタは計算から除外します。
- confidence が高いほど、多くの人が同じデータを得ていることを示します。一致しない場合でも、プレスが異なる盤では正しくリッピングできていることがあります。
- ディスクがデータベースにない (HTTP 404)、ネットワークに接続できないといった場合はメッセージを表示するだけで、リッピング結果や終了コードには影響しません。
- `cdreader toc` でも AccurateRip ディスク ID を表示します。

### 読み取りオフセットの自動検出

`cdreader offset D:` は、AccurateRip に登録されているディスク (よく売れた CD ほど登録が多い) を入れて実行すると、
1 トラックを前後に広めに読み取り、-3000〜+3000 サンプルの各オフセットでチェックサムを計算してデータベースと照合し、一致したオフセットを表示します。

```
Checking track 3 (confidence 25) at offsets -3000..+3000
  offset    +6  matches 25 submission(s)

Read offset: +6  (use: cdreader rip D: --offset 6)
```

- `-t <n>` で照合に使うトラックを、`--range <n>` で探索範囲を指定できます (既定はデータベースの登録が多い中間のトラック、±3000 サンプル)。
- 複数のオフセットが一致した場合は別プレスの登録が混在しています。confidence の高いものを採用し、別のディスクでも確認してください。
- 照合は v1 チェックサムで行います (v2 のみが登録されているディスクでは検出できないことがあります)。

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
AccurateRip はネットワークに接続せず (偽の `HttpClient` を使用)、実在のディスクの ID・データベース応答と、独立した参照実装で求めたチェックサムで検証します。

## 構成

```
core/       プラットフォーム非依存のコア (Windows / Android で共有)
  scsi      ScsiTransport インターフェース、センスデータ解析
  cd_drive  MMC コマンド (INQUIRY, TEST UNIT READY, READ TOC, READ CD)
  toc       TOC 解析、CDDB ID
  accuraterip  AccurateRip ディスク ID・チェックサム v1/v2・データベース応答の解析と照合・オフセット検出
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
- [x] AccurateRip 対応 (照合、オフセット値の自動検出) — [#5](https://github.com/noribow/cdreader/issues/5)
- [ ] リードイン/リードアウトのオーバーリード
- [ ] CDDB 対応 (ディスク情報の取得) — [#6](https://github.com/noribow/cdreader/issues/6)
- [ ] オーディオコーデック対応 (FLAC など) — [#7](https://github.com/noribow/cdreader/issues/7)
- [ ] コンテナフォーマット対応・タグ付け — [#8](https://github.com/noribow/cdreader/issues/8)
- [ ] セキュアモードでのドライブキャッシュ回避 (現状の `--verify` はキャッシュされたデータを再読込する可能性があります)
- [ ] Windows GUI

## 開発の進め方

要求・機能は [GitHub issue](https://github.com/noribow/cdreader/issues) で管理し、PR には対応する issue を記載します (`Closes #番号`)。
詳しくは [CLAUDE.md](CLAUDE.md) を参照してください。

## ライセンス

[BSD 2-Clause License](LICENSE)
