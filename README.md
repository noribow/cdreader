# cdreader

音楽 CD (CD-DA) をリッピングして WAV / FLAC ファイルに保存するツールです。
対象環境は **Windows** と **Android** で、まずは Windows 版 (コマンドライン) から実装しています。

> **このプロジェクトは [Claude Code](https://claude.com/claude-code) (Anthropic の AI コーディングエージェント) を利用して開発しています。**
> 設計・コード・テスト・ドキュメントの多くは Claude Code によって生成され、人がレビューしています。

## 特徴

- SCSI/MMC コマンド (`READ TOC`, `READ CD`) でドライブから直接オーディオセクタを読み取り
- 44.1 kHz / 16 bit / ステレオの WAV、または可逆圧縮の FLAC で保存 (トラックごとに `NN - 曲名.wav` / `NN - 曲名.flac`、曲名が不明なら `TrackNN.wav`)
- FLAC エンコーダは外部ライブラリを使わない自前実装 (Android でもそのままビルド可能)。MD5 署名・タグ・シークテーブル付き
- `--single-file` で全トラックを 1 ファイルのイメージとして保存
- WAV へのタグ書き込み (RIFF `LIST`/`INFO` と ID3v2.4 の `id3 ` チャンク、UTF-8)
- CUE シートの出力 (シングルファイル / トラックごとのどちらでも)
- 読み取りエラー時のリトライ、失敗したブロックはセクタ単位で再読込し、読めないセクタだけを無音で補完
- `--verify` で 2 回読みして一致を確認 (セキュアモード)
- ドライブの読み取りオフセット補正 (`--offset`、EAC / AccurateRip と同じ値)
- トラックごとの CRC32 と `rip.log` (TOC・結果) を出力
- [AccurateRip](https://www.accuraterip.com/) データベースとの照合 (チェックサム v1 / v2、confidence を表示)
- AccurateRip を使ったドライブの読み取りオフセットの自動検出 (`cdreader offset`)
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
cdreader rip D: --single-file       全トラックを 1 つの WAV と CUE シートに保存
cdreader rip D: -f flac             FLAC で保存
cdreader rip D: --cddb-match 2      CDDB の候補が複数あるとき 2 番目を使う
cdreader rip D: --no-cddb           CDDB に問い合わせない (cd_<CDDB ID>\TrackNN.wav)
cdreader rip D: --no-accuraterip    AccurateRip の照合 (ネットワークアクセス) をしない
cdreader offset D:                  読み取りオフセットを AccurateRip で検出
```

| オプション | 説明 |
| --- | --- |
| `-o, --output <dir>` | 出力先ディレクトリ (既定: `アーティスト - アルバム`。CDDB で見つからなければ `cd_<CDDB ID>`) |
| `-f, --format <name>` | 出力フォーマット: `wav` / `flac` (既定: `wav`) |
| `-t, --tracks <list>` | リッピングするトラック (例: `1,3-5`)。既定は全オーディオトラック |
| `-r, --retries <n>` | 読み取り失敗時のリトライ回数 (既定: 5) |
| `--verify` | 全ブロックを 2 回読みして比較 (低速) |
| `--offset <n>` | 読み取りオフセット補正 (サンプル単位、負の値も可。既定: 0) |
| `--single-file` | 選択したトラックを 1 つのファイルにつなげて保存 (ディスクイメージ + CUE シート。FLAC ではファイル内にも CUE シートを埋め込み) |
| `--no-cue-file` | 外部の `.cue` ファイルを書かない |
| `--no-accuraterip` | リッピング後に AccurateRip データベースを照会しない (チェックサムは `rip.log` に記録されます) |

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

### FLAC 出力

`--format flac` を指定すると、各トラックを FLAC (可逆圧縮、[RFC 9639](https://www.rfc-editor.org/rfc/rfc9639)) で保存します。
デコードすると元の PCM とビット単位で一致します。圧縮率は公式 `flac` コマンドの既定 (`-5`) とほぼ同等です。

- エンコーダは `core/` 内の自前実装で、外部ライブラリに依存しません。
  ブロックサイズ 4096 サンプル、サブフレームは CONSTANT / VERBATIM / FIXED (0〜4 次) / LPC (最大 8 次) から最小のものを選択、
  ステレオ相関 (L/R・L/S・S/R・M/S) も最小のものを選択、Rice 符号のパーティション分割とパラメータ探索 (エスケープ符号を含む)、wasted bits に対応。
- メタデータ: `STREAMINFO` (PCM の MD5 署名を含む)、`VORBIS_COMMENT`、`SEEKTABLE` (10 秒ごと)、`PADDING` (タグの後からの書き換え用)。
- タグ (`VORBIS_COMMENT`): `TITLE`, `ARTIST`, `ALBUM`, `ALBUMARTIST`, `TRACKNUMBER`, `TRACKTOTAL`, `DATE`, `GENRE`, `CDDB` (ディスク ID)。
  曲名・アーティストなどは CDDB ([#6](https://github.com/noribow/cdreader/issues/6)) で取得できた場合に入ります。
- `flac -t ファイル名` で CRC と MD5 を検証できます。

#### FLAC への CUE シート埋め込み (internal cue)

`--single-file --format flac` では、外部の `.cue` に加えて CUE シートを FLAC ファイル内に埋め込みます
(1 つの `.flac` だけで各トラックの位置が分かるので、foobar2000 などでトラックごとに再生・分割できます)。

- **`CUESHEET` メタデータブロック** (FLAC ネイティブ): 各トラックの開始位置 (`INDEX 01`、サンプル単位)、リードアウト (トラック 170)、
  CD-DA フラグ、リードイン (88200 サンプル = 2 秒)、プリエンファシスフラグ。libFLAC を使うソフトや `metaflac --export-cuesheet-to=- ファイル名` で読めます。
  リードアウトは実際に書いた音声の長さに合わせて、ファイルを閉じるときに確定します。
- **`CUESHEET` タグ** (Vorbis コメント): 外部 `.cue` と同じテキスト (BOM なし) で、曲名・アーティストなども含みます。foobar2000 などが読みます。
- `--no-cue-file` を付けると外部の `.cue` を書きません (埋め込みのみ)。
- MCN (カタログ番号) と ISRC は現状読み取っていないため空です。プリギャップ (`INDEX 00`) も含みません。
- トラックごとのリッピング (`--single-file` なし) では埋め込みません (1 ファイル 1 トラックのため)。

### AccurateRip による照合

リッピングが終わると、AccurateRip データベース (`http://www.accuraterip.com/accuraterip/.../dBAR-<トラック数>-<ID1>-<ID2>-<CDDB ID>.bin`)
を取得し、各トラックのチェックサムを世界中のほかのユーザーのリッピング結果と照合します。結果はコンソールと `rip.log` に出力されます。

```
AccurateRip (disc id 011-0010e21a-00b8b1b7-950bd70b), 2 pressing(s) in database
Track 01  v1 1A2B3C4D  v2 5E6F7081  Accurately ripped with v2 (v2 12, v1 0 of 15 submissions; 1 of 2 pressings)
          pressing 1: 5E6F7081  confidence  12  v2 match
          pressing 2: 9C8B7A69  confidence   3  no match
Track 02  v1 ...       v2 ...       Not accurate (v2 0, v1 0 of 14 submissions; 0 of 2 pressings)
          pressing 1: ...
Track 03  v1 ...       v2 ...       Not in database
AccurateRip: 1 of 3 track(s) accurately ripped (v2: 1), 2 track(s) in database
```

| 表示 | 意味 |
| --- | --- |
| `Accurately ripped with v1 / v2 / v1+v2` | データベースのチェックサムと一致。どの種類のチェックサム (v1 / v2) と一致したかを表示 (別々のプレスがそれぞれ v1・v2 で一致した場合は `v1+v2`) |
| `v2 N, v1 N of M submissions` | v2 / v1 それぞれで一致した登録数 (confidence) と、このトラックの登録数の合計 M |
| `X of Y pressings` | データベースにあるプレス (盤の種類) Y 件のうち、一致したプレスの数 X |
| `pressing n: <チェックサム> confidence <件数> <結果>` | プレスごとの内訳 (参考情報): 登録されているチェックサム、その登録数、`v2 match` / `v1 match` / `no match` |
| `Not accurate` | 登録はあるが一致しない。読み取りエラー、読み取りオフセットの誤り、別プレス (別の盤) の可能性 |
| `Not in database` | このトラックの登録がない |

最後の行には、一致したトラック数を v1 / v2 別に表示します。
AccurateRip v2 は v1 の計算上の弱点 (サンプル値と位置の積の上位 32 ビットを捨てていた) を直したチェックサムで、データベースの登録には v1 / v2 の区別がないため、両方を計算して照合しています。

- ディスクの識別にはオーディオトラックの開始位置とリードアウト位置から計算した AccurateRip ディスク ID を使います。
  CD-Extra (エンハンスド CD) ではデータトラックを除いたオーディオトラックと、ディスク全体 (データセッション込み) のリードアウトを使います (dBpoweramp / EAC と同じ)。
- チェックサムはオフセット補正後のデータで計算し、ディスク先頭のトラックの最初の 5 セクタ (正確には 2939 サンプル) と最後のトラックの最後の 5 セクタは計算から除外します。
- confidence が高いほど、多くの人が同じデータを得ていることを示します。一致しない場合でも、プレスが異なる盤では正しくリッピングできていることがあります。
- ディスクがデータベースにない (HTTP 404)、ネットワークに接続できないといった場合はメッセージを表示するだけで、リッピング結果や終了コードには影響しません。
- `cdreader toc` でも AccurateRip ディスク ID を表示します。

### 読み取りオフセットの自動検出

`cdreader offset D:` は、AccurateRip に登録されているディスク (よく売れた CD ほど登録が多い) を入れて実行すると、
1 トラックを前後に広めに読み取り、-3000〜+3000 サンプルの各オフセットで v1 / v2 チェックサムを計算してデータベースと照合し、一致したオフセットを表示します。

```
Checking track 3 (confidence 25) at offsets -3000..+3000
Matching offsets (submissions whose checksum matches at that offset):
  offset    +6  v1+v2  v2  18  v1   7  (2 of 3 pressing(s))
  offset  +667  v2     v2   2  v1   0  (1 of 3 pressing(s))

Read offset: +6  (matched v1+v2, confidence 25; use: cdreader rip D: --offset 6)
```

- `-t <n>` で照合に使うトラックを、`--range <n>` で探索範囲を指定できます (既定はデータベースの登録が多い中間のトラック、±3000 サンプル)。
- 複数のオフセットが一致した場合は別プレスの登録が混在しています。confidence の高いものを採用し、別のディスクでも確認してください。
- v1 / v2 の両方で照合し、オフセットごとに v1 / v2 それぞれの一致数と一致したプレスの数を表示します。
- v1 は全オフセット分を 1 回のスライド計算で求めます。v2 はサンプル値と位置の積の上位 32 ビットを含むためスライド計算できませんが、
  2^32 ≡ 1 (mod 2^32 − 1) を使った剰余のスライド計算で一致の可能性がないオフセットを除外し、残った少数のオフセットだけ厳密に計算します
  (5 分のトラック・±3000 サンプル・3 プレスで約 1 秒)。
- 検出中は読み取ったトラックをメモリに保持します (1 サンプル 4 バイト、5 分のトラックで約 50 MB)。

### タグ

トラックのメタデータ (タイトル・アーティスト・アルバム・トラック番号・年・ジャンル・CDDB ディスク ID) を WAV に書き込みます。
タイトルなどは CDDB ([#6](https://github.com/noribow/cdreader/issues/6)) で取得できた場合に入ります (取得できなければトラック番号とディスク ID のみ)。
FLAC のタグは `VORBIS_COMMENT` に書きます (FLAC 出力の節を参照)。

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
FLAC は MD5 / CRC / ビット書き込み / Rice 符号の単体テストと、テスト用の簡易デコーダ (`tests/flac_decoder.*`) による往復テストに加え、
公式 `flac` コマンドがインストールされていれば (`apt-get install flac` など)、さまざまな合成信号を `flac -t` で検証・`flac -d` でデコードして
元の PCM と一致することを確認するテスト (`flac_roundtrip`) と、埋め込み CUE シートを `metaflac` で読み出し・再取り込みして確認するテスト (`flac_cuesheet`) も実行されます (無い場合はスキップ)。
CDDB は偽の `HttpClient` を使い (ネットワークには接続しません)、問い合わせコマンドの生成・応答コード・xmcd エントリの解析・ファイル名の変換を検証します。
AccurateRip はネットワークに接続せず (偽の `HttpClient` を使用)、実在のディスクの ID・データベース応答と、独立した参照実装で求めたチェックサムで検証します。

## 構成

```
core/       プラットフォーム非依存のコア (Windows / Android で共有)
  scsi      ScsiTransport インターフェース、センスデータ解析
  cd_drive  MMC コマンド (INQUIRY, TEST UNIT READY, READ TOC, READ CD)
  toc       TOC 解析、CDDB ID
  cddb      CDDB の問い合わせ・応答解析 (HttpClient 経由)
  file_naming  メタデータからのファイル名・フォルダ名・アルバム単位のファイル名 (使えない文字の置換)
  accuraterip  AccurateRip ディスク ID・チェックサム v1/v2・データベース応答の解析と照合・オフセット検出
  ripper    リトライ・セクタ分割・verify を含むトラック読み取り
  audio_writer  出力フォーマットの共通インターフェース (WAV など。コーデック・コンテナはここに追加)
  metadata  アルバム / トラック情報 (タグ付け・ファイル名用)
  tags      RIFF INFO / ID3v2.4 タグの生成
  cue_sheet CUE シートの生成
  ogg       Ogg コンテナ (ページ分割・CRC。Ogg Opus / Ogg FLAC 用、コーデック非依存)
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
- [x] AccurateRip 対応 (照合、オフセット値の自動検出) — [#5](https://github.com/noribow/cdreader/issues/5)
- [ ] リードイン/リードアウトのオーバーリード
- [x] CDDB 対応 (ディスク情報の取得) — [#6](https://github.com/noribow/cdreader/issues/6)
- [ ] MusicBrainz 対応、CD-TEXT の読み取り (CDDB に無いディスクの情報取得)
- [x] オーディオコーデック対応: FLAC — [#7](https://github.com/noribow/cdreader/issues/7)
- [ ] 非可逆コーデック (MP3 / AAC / Opus / Vorbis) — [#13](https://github.com/noribow/cdreader/issues/13)
- [ ] FLAC の圧縮レベル指定 (`--compression` など。現状は `flac -5` 相当の固定設定)
- [ ] コンテナフォーマット対応・タグ付け — [#8](https://github.com/noribow/cdreader/issues/8)
  - [x] WAV のタグ (INFO / ID3)、シングルファイル + CUE シート、Ogg ページ書き出し (コア)
  - [x] FLAC への CUE シート埋め込み (CUESHEET ブロック + タグ) — [#16](https://github.com/noribow/cdreader/issues/16)
  - [ ] MCN / ISRC の読み取り (CUE シート・FLAC の CUESHEET への記録)
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
