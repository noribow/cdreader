# cdreader

音楽 CD (CD-DA) をリッピングして WAV / FLAC ファイルに保存するツールです。
対象環境は **Windows** と **Android** で、まずは Windows 版 (コマンドライン) から実装しています。

> **このプロジェクトは [Claude Code](https://claude.com/claude-code) (Anthropic の AI コーディングエージェント) を利用して開発しています。**
> 設計・コード・テスト・ドキュメントの多くは Claude Code によって生成され、人がレビューしています。

## 特徴

- SCSI/MMC コマンド (`READ TOC`, `READ CD`) でドライブから直接オーディオセクタを読み取り
- 44.1 kHz / 16 bit / ステレオの WAV、または可逆圧縮の FLAC で保存 (トラックごとに `TrackNN.wav` / `TrackNN.flac`)
- FLAC エンコーダは外部ライブラリを使わない自前実装 (Android でもそのままビルド可能)。MD5 署名・タグ・シークテーブル付き
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
cdreader rip D: -f flac             FLAC で保存
```

| オプション | 説明 |
| --- | --- |
| `-o, --output <dir>` | 出力先ディレクトリ (既定: `cd_<CDDB ID>`) |
| `-f, --format <name>` | 出力フォーマット: `wav` / `flac` (既定: `wav`) |
| `-t, --tracks <list>` | リッピングするトラック (例: `1,3-5`)。既定は全オーディオトラック |
| `-r, --retries <n>` | 読み取り失敗時のリトライ回数 (既定: 5) |
| `--verify` | 全ブロックを 2 回読みして比較 (低速) |
| `--offset <n>` | 読み取りオフセット補正 (サンプル単位、負の値も可。既定: 0) |

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

### FLAC 出力

`--format flac` を指定すると、各トラックを FLAC (可逆圧縮、[RFC 9639](https://www.rfc-editor.org/rfc/rfc9639)) で保存します。
デコードすると元の PCM とビット単位で一致します。圧縮率は公式 `flac` コマンドの既定 (`-5`) とほぼ同等です。

- エンコーダは `core/` 内の自前実装で、外部ライブラリに依存しません。
  ブロックサイズ 4096 サンプル、サブフレームは CONSTANT / VERBATIM / FIXED (0〜4 次) / LPC (最大 8 次) から最小のものを選択、
  ステレオ相関 (L/R・L/S・S/R・M/S) も最小のものを選択、Rice 符号のパーティション分割とパラメータ探索 (エスケープ符号を含む)、wasted bits に対応。
- メタデータ: `STREAMINFO` (PCM の MD5 署名を含む)、`VORBIS_COMMENT`、`SEEKTABLE` (10 秒ごと)、`PADDING` (タグの後からの書き換え用)。
- タグ (`VORBIS_COMMENT`): `TITLE`, `ARTIST`, `ALBUM`, `ALBUMARTIST`, `TRACKNUMBER`, `TRACKTOTAL`, `DATE`, `GENRE`, `CDDB` (ディスク ID)。
  現状 CLI が設定するのは `TRACKNUMBER` / `TRACKTOTAL` / `CDDB` で、曲名などは CDDB 対応 ([#6](https://github.com/noribow/cdreader/issues/6)) 後に入ります。
- `flac -t ファイル名` で CRC と MD5 を検証できます。

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
FLAC は MD5 / CRC / ビット書き込み / Rice 符号の単体テストと、テスト用の簡易デコーダ (`tests/flac_decoder.*`) による往復テストに加え、
公式 `flac` コマンドがインストールされていれば (`apt-get install flac` など)、さまざまな合成信号を `flac -t` で検証・`flac -d` でデコードして
元の PCM と一致することを確認するテスト (`flac_roundtrip`) も実行されます (無い場合はスキップ)。

## 構成

```
core/       プラットフォーム非依存のコア (Windows / Android で共有)
  scsi      ScsiTransport インターフェース、センスデータ解析
  cd_drive  MMC コマンド (INQUIRY, TEST UNIT READY, READ TOC, READ CD)
  toc       TOC 解析、CDDB ID
  ripper    リトライ・セクタ分割・verify を含むトラック読み取り
  audio_writer  出力フォーマットの共通インターフェース (WAV など。コーデック・コンテナはここに追加)
  metadata  アルバム / トラック情報 (タグ付け・ファイル名用)
  http      オンライン照会用 HTTP インターフェース (実装はプラットフォーム側)
  wav_writer, crc32
  flac_writer, flac_encoder, md5  FLAC エンコーダ (外部ライブラリなし)
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
- [x] オーディオコーデック対応: FLAC — [#7](https://github.com/noribow/cdreader/issues/7)
- [ ] 非可逆コーデック (MP3 / AAC / Opus / Vorbis)。外部ライブラリ (LAME, libopus など) が必要なため、ライセンスと Android 向けビルドを検討のうえ別途対応
- [ ] FLAC の圧縮レベル指定 (`--compression` など。現状は `flac -5` 相当の固定設定)
- [ ] コンテナフォーマット対応・タグ付け — [#8](https://github.com/noribow/cdreader/issues/8)
- [ ] セキュアモードでのドライブキャッシュ回避 (現状の `--verify` はキャッシュされたデータを再読込する可能性があります)
- [ ] Windows GUI

## 開発の進め方

要求・機能は [GitHub issue](https://github.com/noribow/cdreader/issues) で管理し、PR には対応する issue を記載します (`Closes #番号`)。
詳しくは [CLAUDE.md](CLAUDE.md) を参照してください。

## ライセンス

[BSD 2-Clause License](LICENSE)
