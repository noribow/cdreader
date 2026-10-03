# cdreader

音楽 CD (CD-DA) をリッピングして WAV / FLAC / Ogg FLAC / ALAC (M4A) / Opus / Ogg Vorbis / Matroska (MKA) ファイルに保存するツールです。
対象環境は **Windows** (コマンドライン) と **Android** (USB 接続の外付け CD ドライブ) です。

> **このプロジェクトは [Claude Code](https://claude.com/claude-code) (Anthropic の AI コーディングエージェント) を利用して開発しています。**
> 設計・コード・テスト・ドキュメントの多くは Claude Code によって生成され、人がレビューしています。

## 特徴

- SCSI/MMC コマンド (`READ TOC`, `READ CD`, `READ SUB-CHANNEL`) でドライブから直接オーディオセクタを読み取り
- 44.1 kHz / 16 bit / ステレオの WAV、または可逆圧縮の FLAC で保存 (トラックごとに `NN - 曲名.wav` / `NN - 曲名.flac`、曲名が不明なら `TrackNN.wav`)
- FLAC エンコーダは外部ライブラリを使わない自前実装 (Android でもそのままビルド可能)。MD5 署名・タグ・シークテーブル付き
- 同じ FLAC を Ogg コンテナに入れた **Ogg FLAC** (`.oga`) でも保存可能 (可逆圧縮、FLAC-to-Ogg マッピング 1.0)
- 可逆圧縮の **ALAC** (Apple Lossless、MP4 コンテナの `.m4a`、iTunes 形式のタグ付き) でも保存可能。Apple 製品 (ミュージック・iPhone など) 向けの形式で、
  エンコーダと MP4 の書き出しも外部ライブラリなしの自前実装
- 非可逆圧縮の **Opus** (Ogg Opus、`.opus`) と **Vorbis** (Ogg Vorbis、`.ogg`) でも保存可能 (libopus / libvorbis を使用。ビットレート・品質を指定可能)
- **Matroska** (`.mka`) コンテナでも保存可能 (中身は FLAC / PCM / Opus / Vorbis から選択。タグ付き、シングルファイルではトラックごとのチャプター付き)
- `--single-file` で全トラックを 1 ファイルのイメージとして保存
- WAV へのタグ書き込み (RIFF `LIST`/`INFO` と ID3v2.4 の `id3 ` チャンク、UTF-8)
- CUE シートの出力 (シングルファイル / トラックごとのどちらでも)
- ディスクの MCN (カタログ番号、JAN / UPC) とトラックごとの ISRC をサブチャンネル Q から読み取り、CUE シート・FLAC の `CUESHEET`・タグ・`rip.log` に記録
- トラック間のプリギャップ (`INDEX 00`)・`INDEX 02` 以降と、1 曲目の前の隠しトラック (HTOA) をサブチャンネル Q から検出し、
  CUE シート・FLAC の `CUESHEET`・`rip.log` に反映 (HTOA はシングルファイルのイメージに含め、`--htoa` でトラック 00 としても保存)
- 読み取りエラー時のリトライ、失敗したブロックはセクタ単位で再読込し、読めないセクタだけを無音で補完
- `--verify` で 2 回読みして一致を確認 (セキュアモード)
- 対応ドライブでは **C2 エラーポインタ**で訂正しきれなかったセクタを検出して再読込し、解決しなかった位置を
  「疑わしい位置」(Suspicious position) として `rip.log` に記録 (既定で有効、`--no-c2` で無効)
- **ドライブキャッシュ対策**: `--verify` の 2 回目・リトライ・C2 の再読込の前に、ドライブのキャッシュではなくディスクから
  読み直させる (FUA または追い出し。既定はドライブを試験して自動選択、`--cache` で指定)
- ドライブの読み取りオフセット補正 (`--offset`、EAC / AccurateRip と同じ値)
- トラックごとの CRC32 と `rip.log` (TOC・結果) を出力
- [AccurateRip](https://www.accuraterip.com/) データベースとの照合 (チェックサム v1 / v2、confidence を表示)
- AccurateRip を使ったドライブの読み取りオフセットの自動検出 (`cdreader offset`、`rip --offset auto`、Android は詳細設定)。
  複数トラックが同じオフセットで一致したときだけ確定し (互いにずれた別プレスの登録が混在するディスクでは一致件数が十分に多いオフセットで確定)、ドライブ (ベンダー・モデル・リビジョン) ごとに保存して次回から自動で適用
- CD-Extra (エンハンスド CD) のデータトラック・セッション間ギャップを考慮
- CDDB (既定は [gnudb.org](https://gnudb.org/)) からアルバム名・アーティスト・曲名などを取得し、
  フォルダ名・ファイル名・`rip.log` に使用

## 使い方 (Windows)

```
cdreader drives                     光学ドライブの一覧 (C2 エラーポインタ対応の有無とキャッシュサイズも表示)
cdreader toc D:                     TOC (トラック一覧)、MCN / ISRC と CDDB のディスク情報を表示
cdreader rip D:                     全オーディオトラックを "アーティスト - アルバム"\ に保存
cdreader rip D: -t 1,3-5 -o out     トラック 1,3,4,5 を out\ に保存
cdreader rip D: --verify -r 10      2 回読み比較、リトライ 10 回
cdreader rip D: --offset 6          読み取りオフセット +6 サンプルで補正
cdreader rip D: --offset auto       このドライブの保存済みオフセットを使う (なければ自動検出して保存)
cdreader rip D: --no-c2             C2 エラーポインタを使わずに読み取る
cdreader rip D: --verify --cache flush  2 回読み比較、キャッシュは追い出しで回避
cdreader rip D: --single-file       全トラックを 1 つの WAV と CUE シートに保存
cdreader rip D: -f flac             FLAC で保存
cdreader rip D: -f oggflac          Ogg FLAC (.oga) で保存
cdreader rip D: -f alac             ALAC (Apple Lossless、.m4a) で保存
cdreader rip D: -f opus             Opus で保存 (VBR 160 kbit/s)
cdreader rip D: -f opus -b 128      Opus 128 kbit/s で保存
cdreader rip D: -f vorbis -q 6      Ogg Vorbis 品質 6 で保存
cdreader rip D: -f mka              Matroska (中身は FLAC) で保存
cdreader rip D: -f mka-opus -b 128  Matroska (中身は Opus 128 kbit/s) で保存
cdreader rip D: -f mka --single-file  1 つの MKA (トラックごとのチャプター付き) と CUE シートに保存
cdreader rip D: --cddb-match 2      CDDB の候補が複数あるとき 2 番目を使う
cdreader rip D: --no-cddb           CDDB に問い合わせない (cd_<CDDB ID>\TrackNN.wav)
cdreader rip D: --no-accuraterip    AccurateRip の照合 (ネットワークアクセス) をしない
cdreader rip D: --no-isrc           MCN / ISRC を読み取らない (読み取りの遅いドライブ向け)
cdreader rip D: --no-gaps           プリギャップ (INDEX 00) を検出しない (時間短縮)
cdreader rip D: -f flac --htoa      1 曲目の前の隠しトラック (HTOA) も 00 - Hidden Track.flac として保存
cdreader offset D:                  読み取りオフセットを AccurateRip で検出
cdreader offset D: --save           検出したオフセットをこのドライブの値として保存
cdreader offsets                    保存済みのオフセット (ドライブごと) の一覧
cdreader config cddb-email you@example.com  CDDB に送る連絡先メールアドレスを保存 (gnudb で必要)
cdreader config cddb-server japdb   CDDB サーバーに JAPDB を使う (gnudb / japdb / URL)
cdreader rip D: --cddb-server japdb  今回だけ JAPDB で検索する
cdreader config                     保存済みの設定の一覧
cdreader cddb-test                  CDDB サーバーへの接続テスト
```

| オプション | 説明 |
| --- | --- |
| `-o, --output <dir>` | 出力先ディレクトリ (既定: `アーティスト - アルバム`。CDDB で見つからなければ `cd_<CDDB ID>`) |
| `-f, --format <name>` | 出力フォーマット: `wav` / `flac` / `oggflac` / `alac` / `opus` / `vorbis` / `mka` / `mka-pcm` / `mka-opus` / `mka-vorbis` (既定: `wav`。`alac` は `.m4a`。`opus` / `vorbis` / `mka-opus` / `mka-vorbis` はビルド時に有効にした場合のみ。MKA は「Matroska (MKA) 出力」の節を参照) |
| `-b, --bitrate <kbps>` | 非可逆フォーマットの目標ビットレート (kbit/s、VBR)。`opus` / `mka-opus`: 6〜510 (既定 160)、`vorbis` / `mka-vorbis`: 45〜500 (平均ビットレート、`--quality` の代わり) |
| `-q, --quality <q>` | `vorbis` / `mka-vorbis` の VBR 品質 (oggenc と同じ -1〜10、小数可。既定 5 ≒ 160 kbit/s)。Opus には指定できません |
| `-t, --tracks <list>` | リッピングするトラック (例: `1,3-5`)。既定は全オーディオトラック |
| `-r, --retries <n>` | 読み取り失敗時のリトライ回数 (既定: 5)。C2 エラーのあるセクタの再読込回数の上限も兼ねます |
| `--verify` | 全ブロックを 2 回読みして比較 (低速) |
| `--no-c2` | C2 エラーポインタを使わない (既定では対応ドライブなら使います。「C2 エラーポインタ」の節を参照) |
| `--cache <mode>` | 再読込の前のドライブキャッシュ対策: `auto` (既定。ディスクごとにドライブを試験して選ぶ)・`fua`・`flush` (追い出し)・`none` (しない)。「ドライブキャッシュ対策」の節を参照 |
| `--offset <n>` | 読み取りオフセット補正 (サンプル単位、負の値も可。既定: 0) |
| `--offset auto` | このドライブの保存済みオフセットを使う。保存されていなければ AccurateRip で自動検出して保存してから読み取る (検出できなければリッピングせずに終了)。「読み取りオフセットの自動検出」の節を参照 |
| `--single-file` | 選択したトラックを 1 つのファイルにつなげて保存 (ディスクイメージ + CUE シート。FLAC / Ogg FLAC ではファイル内にも CUE シートを埋め込み、MKA ではトラックごとのチャプターを記録) |
| `--no-cue-file` | 外部の `.cue` ファイルを書かない |
| `--no-accuraterip` | リッピング後に AccurateRip データベースを照会しない (チェックサムは `rip.log` に記録されます) |
| `--no-isrc` | MCN (カタログ番号) と ISRC を読み取らない (`toc` でも使えます)。読み取りに時間のかかるドライブ向け |
| `--no-gaps` | プリギャップ (`INDEX 00`)・`INDEX 02` 以降の検出をしない (`toc` でも使えます)。CUE シートは `INDEX 01` のみになります。HTOA は TOC から分かるため従来どおり扱います |
| `--htoa` | トラックごとのリッピングで、1 曲目の前の隠しトラック (HTOA) をトラック 00 (`00 - Hidden Track.<拡張子>` / `Track00.<拡張子>`) としても保存する (シングルファイルでは常にイメージに含めます) |

CDDB のオプション (`rip` と `toc` の両方で使えます。`--cddb-server` と `--cddb-hello` は `cddb-test` でも使えます。
保存済みの設定 (「CDDB 設定」の節) より優先されます):

| オプション | 説明 |
| --- | --- |
| `--no-cddb` | CDDB に問い合わせない |
| `--cddb-server <name\|url>` | CDDB サーバー。プリセット名 (`gnudb` / `japdb`、大文字小文字は区別しない) または HTTP / HTTPS の CGI の URL (既定: 設定 `cddb-server`、なければ `gnudb`。「CDDB 設定」の節を参照) |
| `--cddb-match <n>` | 候補が複数あるときに使う候補の番号 (1 から。既定: 1) |
| `--cddb-hello <user@host>` | CDDB の hello に送る連絡先メールアドレス (`@` の前がユーザー名、後ろがホスト名として送られます。既定: 設定 `cddb-email`、なければ匿名の `cdreader@localhost`) |

設定のコマンド ([#38](https://github.com/noribow/cdreader/issues/38)):

| コマンド | 説明 |
| --- | --- |
| `cdreader config` | 保存済みの設定と既定値の一覧 |
| `cdreader config <key>` | 1 つの設定の値を表示 |
| `cdreader config <key> <value>` | 設定を検証して保存 |
| `cdreader config <key> --unset` | 既定値に戻す |
| `cdreader cddb-test [--cddb-server <name\|url>] [--cddb-hello <user@host>]` | CDDB サーバーへの接続テスト (`stat` コマンドを 1 回送り、サーバーの応答を表示。終了コード 0 成功 / 1 失敗) |

| 設定 (`<key>`) | 内容 |
| --- | --- |
| `cddb-server` | CDDB サーバー: プリセット名 (`gnudb` / `japdb`) または URL (`http://` または `https://`)。プリセットの URL を指定した場合もプリセット名で保存します |
| `cddb-email` | CDDB の hello に送る連絡先メールアドレス (`user@host` 形式) |
| `cddb-app-name` / `cddb-app-version` | hello に送るアプリ名 / バージョン (既定: `cdreader` / プログラムのバージョン。通常は変更不要) |

終了コード: `0` 成功 / `1` エラー / `2` 読めないセクタ、または疑わしい位置 (C2) があった。

### CDDB とファイル名

`rip` / `toc` は TOC から freedb 形式のディスク ID を計算し、CDDB サーバーに `cddb query` → `cddb read` (HTTP, proto 6 = UTF-8) で問い合わせます。

- 候補が複数ある場合 (完全一致が複数、または近似一致) は 1 番目を使い、全候補を画面と `rip.log` に表示します。
  別の候補を使うには `--cddb-match <n>` を指定します (`cdreader toc D:` で候補を確認できます)。
- 取得したアルバム名・アーティスト・年・ジャンル・曲名は `rip.log` に記録され、出力ファイルのメタデータとして渡されます
  (タグの書き込みは出力フォーマット側の対応次第です。WAV には現在タグを書きません)。
- コンピレーション (`Various Artists` など、曲名が `アーティスト / 曲名` 形式) は曲ごとのアーティストに分けます。
- UTF-8 として不正なデータ (古い Latin-1 のエントリ) は Latin-1 として読み替えます (Shift_JIS など他の文字コードで登録されたエントリは文字化けします)。
- 通信エラーや該当なしでもリッピングは中断せず、従来どおり `cd_<CDDB ID>\TrackNN.wav` に保存します。
- サーバーが hello (挨拶) を拒否した場合は、画面と `rip.log` に対処方法 (`Hint: ... set a contact e-mail address in the CDDB settings ...`) を表示します (次の節を参照)。

### CDDB 設定 (サーバーと連絡先メールアドレス)

#### サーバーの選択 ([#46](https://github.com/noribow/cdreader/issues/46))

CDDB サーバーは次のプリセットから選ぶか、任意の URL (カスタム) を指定します。

| プリセット (名前) | 表示名 | URL | 備考 |
| --- | --- | --- | --- |
| `gnudb` (既定) | GNUDB | `https://gnudb.gnudb.org/~cddb/cddb.cgi` | 連絡先メールアドレスが必要 (下記) |
| `japdb` | JAPDB | `http://freedbtest.dyndns.org:80/~cddb/cddb.cgi` | freedb 互換のサーバー。HTTP (暗号化なし) |

- Windows: `cdreader config cddb-server japdb` (または `gnudb`、`https://...` の URL) で保存し、今回だけ変えるなら `--cddb-server japdb`。
  `cdreader config` の一覧には `cddb-server  japdb (JAPDB, http://...)` のようにプリセットの表示名と URL を表示します。
  `cdreader --help` の「CDDB servers」にプリセットの一覧があります。
- Android: 「詳細設定」→「CDDB」の「サーバー」のドロップダウンで「GNUDB / JAPDB / カスタム」を選びます。
  プリセットを選ぶとその URL をドロップダウンの下に表示し、「カスタム」を選んだときだけ URL の入力欄を表示します。
  以前のバージョンで URL を保存していた場合、それがプリセットの URL (末尾の `/`、既定のポート `:80` / `:443`、
  スキームとホスト名の大文字小文字の違いは無視) ならそのプリセットに、それ以外ならカスタムに自動で切り替わります。
  「接続テスト」は選択中のサーバーに送ります。
- 設定には、プリセットならプリセット名 (`gnudb` / `japdb`)、カスタムなら URL を保存します。
- `rip.log` の CDDB の行は、プリセットのサーバーなら表示名も記録します (例: `CDDB lookup (JAPDB, http://freedbtest.dyndns.org:80/~cddb/cddb.cgi): 1 exact match(es)`)。
- 連絡先メールアドレスの注意 (gnudb が要求) は、サーバーが gnudb.org のときだけ表示します。
- Android の通信設定 (`network_security_config.xml`) は暗号化なしの HTTP を AccurateRip と JAPDB のホスト
  (`freedbtest.dyndns.org`) にだけ許可しています。カスタムに `http://` の他のサーバーを入力すると、
  欄に「https:// の URL を指定してください」と表示して保存しません (Windows 版には制限はありません)。

#### 連絡先メールアドレス

CDDB の問い合わせには毎回 hello (`ユーザー名 ホスト名 アプリ名 バージョン`) を付けます。
既定のサーバー gnudb.org は、利用者 (開発者) の **連絡先メールアドレス** を hello に含めることを求めており、
匿名の hello (`cdreader localhost cdreader 0.1.0`) には次のように応答して検索に応じません:

```
500 Unknown application, developer email for cdreader 0.1.0
```

このため、gnudb を使う場合はご自分の連絡先メールアドレスを設定してください。アドレスは `@` で分けて
hello のユーザー名とホスト名として送られます (例: `you@example.com` → `hello=you example.com cdreader 0.1.0`)。

- Windows: `cdreader config cddb-email you@example.com` で保存します (毎回指定するなら `--cddb-hello you@example.com`)。
  別のサーバーを使うには `cdreader config cddb-server japdb` (または `<url>`)。`cdreader cddb-test` で設定を確認できます。
  設定は `%APPDATA%\cdreader\settings.txt` (読み取りオフセットの `drive_offsets.txt` と同じフォルダ。`APPDATA` が無ければ
  `cdreader.exe` と同じフォルダ) に `key=value` 形式で保存されます。コマンドラインのオプションは保存値より優先されます。
- Android: 「詳細設定」の「CDDB」で「連絡先メールアドレス」を入力します (「Android の使い方」を参照)。

接続テストは CDDB の `stat` コマンド (サーバーの状態表示。ディスクも検索も不要で応答が短い) を、検索と同じ URL・hello・`proto=6` で 1 回送ります。
2xx の応答なら成功です。サーバーが gnudb なのにメールアドレスが未設定の場合は、成功しても注意を表示します
(サーバーが `stat` で hello を確認しない場合、検索だけが拒否されることがあるためです)。

検索や接続テストの失敗が hello に関するもの (応答コード 409 / 431、または `email`・`e-mail`・`hello`・`handshake`・`unknown application` を含む応答)
の場合は、CDDB 設定で連絡先メールアドレスを設定するよう案内します (Windows・`rip.log` は英語、Android の画面は日本語)。

**プライバシーについて**: 入力したメールアドレスは CDDB サーバーに (検索のたびに URL の一部として) 送信されます。
cdreader は既定ではどんな個人のアドレスも送らず、設定しない限り匿名の hello のままです。設定を消せば (`cdreader config cddb-email --unset`、
Android では欄を空にする) 匿名に戻ります。`rip.log` にはアドレスを記録しません。

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
- 値が分からない場合は自動検出できます (`cdreader offset D: --save`、`rip --offset auto`。次の節を参照)。
  `rip.log` には補正値とその出所 (`manual` / `saved for drive …` / `auto-detected: 2 of 2 tracks agreed, v2`) が記録されます。
- 1 サンプル = 4 バイト (16 bit ステレオ)。`+N` の場合、各トラックはドライブが返すデータの N サンプル後ろから切り出されます。
- 補正によってディスクの先頭より前・リードアウトより後 (CD-Extra ではデータセッションとの間) にはみ出した部分は、
  多くのドライブで読めないため無音で埋めます。その数は `rip.log` に記録されます。

### C2 エラーポインタ

CD のデータは CIRC (C1 / C2 の 2 段の誤り訂正) で守られていますが、傷などで C2 段でも訂正しきれないと、
ドライブは前後のサンプルから補間した値を返します。対応ドライブは、どのバイトが訂正できなかったかを
**C2 エラーポインタ** (1 バイトにつき 1 ビット) として報告できます ([#33](https://github.com/noribow/cdreader/issues/33))。

- リッピング開始時に `MODE SENSE` (CD 機能ページ 2Ah の "C2 Pointers are supported" ビット) で対応を確認します。
  `MODE SENSE` に応答しないドライブは非対応として扱います。対応していれば既定で有効です (`--no-c2` で無効)。
  `cdreader drives` / `cdreader toc` にも対応状況を表示します。
- 読み取りは `READ CD` のエラーフィールド 01b (C2 エラーブロックデータ、1 セクタあたり 2352 + 294 バイト) を使います。
  10b (ブロックエラーバイトとパッドを加えた 296 バイト) は使わない情報が増えるだけで、対応していないドライブもあるためです。
  転送量が増えるので、1 コマンドあたりのセクタ数を 26 から 24 に減らしています (64 KiB 未満。SPTI のバッファ・USB ブリッジ向け)。
- C2 エラーのあるセクタは 1 セクタずつ再読込します (最大 `-r` 回)。次のどちらかで終わります:
  - C2 エラーなしで読めた (`--verify` では同じ内容をもう一度読めた場合のみ) → 「回復」
  - 連続する 2 回の読み取りが同じ内容だった (C2 エラーは残ったまま) → 「一致」。ドライブが毎回同じ補間値を返す、または
    誤って C2 を報告しているので、それ以上読んでも変わらないと判断します。`--verify` では再読込の 2 回が一致した場合のみ
    (最初の読み取りは数えません)、それ以外では C2 を報告した最初の読み取りも数えます。
- 上限まで読んでも解決しなかったセクタは、C2 エラーの最も少なかった読み取りを使い、そのバイトを含む出力側のセクタを
  **疑わしい位置**として記録します。位置は出力トラックの先頭からの時間 (EAC と同じ `時:分:秒`) とセクタ番号で、
  読み取りオフセット補正後の位置です (補正でずれたバイトが隣のセクタ・隣のトラックに入る場合もその位置になります)。
  疑わしい位置のあるトラックはエラー扱いです (終了コード `2`、`Finished with errors`)。
- `rip.log` の例:

  ```
  Drive: PLEXTOR DVDR PX-760A (1.07)
  C2 pointers: supported
  ...
  03 - Song.flac  CRC32 1A2B3C4D  retries 0  2 suspicious sector(s)
    C2 errors: 5 sector(s), 13 re-read(s) (3 recovered, 1 identical re-reads with C2, 1 unresolved)
    Suspicious position 0:01:23 - 0:01:23 (sectors 6230-6231)
  ```

- C2 付きの読み取りを ILLEGAL REQUEST で拒否する、またはエラーフィールドを無視してデータが足りないドライブでは、
  同じセクタを通常の読み取りで読めることを確かめたうえで、そのディスクの残りを通常の読み取りに切り替え、`rip.log` に記録します
  (`C2 reads given up: ...`)。
- C2 を使わない場合 (`--no-c2`・非対応ドライブ) の読み取りは従来とまったく同じです (同じコマンド・同じ出力)。
- プリギャップ・HTOA の検出 (サブチャンネルの読み取り) には C2 を使いません。HTOA をトラック 00 として保存する場合の音声の読み取りには使います。

注意:

- **C2 の精度はドライブ依存です。** 報告の漏れ・誤報告・ビット順序の違うドライブもあります。C2 は再読込する箇所の絞り込みに使うもので、
  最終的な正しさは AccurateRip の照合や `--verify` で確認してください。C2 を報告せずに誤ったデータを返す場合は C2 では検出できません
  (毎回違うデータなら `--verify` で、毎回同じなら AccurateRip でのみ分かります)。
- ドライブのキャッシュにより再読込でも同じデータが返る場合がありますが、次の「ドライブキャッシュ対策」で回避します。
- 実機での動作確認はこれからです。

### ドライブキャッシュ対策

多くのドライブは読んだセクタを内蔵メモリ (キャッシュ) に残し、同じセクタをもう一度要求されるとディスクを読まずにキャッシュから返します。
そのままでは `--verify` の 2 回目の読み取り、読み取りエラーのリトライ、セクタ単位の再読込、C2 エラーの再読込が
1 回目と同じ (誤っているかもしれない) データを受け取るだけになり、一時的な読み取りエラーが「一致した」ことになってしまいます
([#34](https://github.com/noribow/cdreader/issues/34))。そこで、1 回目以外の読み取りの前に次のどちらかを行います
(1 回目の読み取りには何もしないので、エラーのないディスクでは出力も読み取り回数も変わりません):

- **FUA** (`--cache fua`): 読み直すセクタの LBA を指定した `READ(12)` を FUA (Force Unit Access) ビット付き・転送長 0 で送ります。
  対応ドライブ (Plextor など) はこれでキャッシュを破棄します。転送長 0 なのでデータは転送されず、音声セクタを `READ(12)` で
  読めないドライブでも問題になりません。速いですが、受け付けても何もしないドライブが多く、`READ(12)` を ILLEGAL REQUEST で
  拒否するドライブもあります (拒否された場合はそのディスクの残りを追い出しに切り替え、`rip.log` に `FUA given up: ...` と記録します)。
- **追い出し** (`--cache flush`): 読み直すセクタから離れた場所 (同じオーディオ領域のなるべく後ろ、後ろに余裕がなければ先頭) を
  キャッシュより多く読んで、キャッシュの中身を入れ替えます。量は `MODE SENSE` ページ 2Ah のバッファサイズ (バイト 12〜13、KB 単位) の
  1.1 倍 (最低でも 1 コマンド分多く)。報告がなければ 4096 KB とみなし、8192 KB を上限にします。リードアウトの先やデータトラックは読みません。
  どのドライブにも効きますが、再読込 1 回ごとに数百〜数千セクタを余分に読むため、`--verify` と組み合わせると大幅に遅くなります。
- **自動** (`--cache auto`、既定): ディスクごとに 1 回 (数秒)、オーディオ領域の 3 か所で 26 セクタの読み取り時間を測ります
  (読んだ直後にもう一度読む / FUA の後に読む / 追い出しの後、すぐ後ろを読んでから読む。それぞれ 3 回の中央値)。
  - 追い出し後の読み取りまでディスクから読めない速さ (1 セクタ 200 µs 未満、67 倍速相当) → 判定不能
  - 直後の再読込がディスクから読めない速さか、追い出し後の 1/3 以下 → キャッシュあり。2/3 以上 → キャッシュなし。その間 → 判定不能
  - キャッシュありの場合、FUA の後の読み取りがキャッシュの時間と追い出し後の時間の幾何平均以上かかれば「FUA が効く」

  結果に応じて、キャッシュなし → 対策なし、FUA が効く → FUA、FUA が効かない・拒否される・判定不能 → 追い出し、を使います。
- **なし** (`--cache none`): 何もしません (#33 までと同じコマンド列)。

`rip.log` の例:

```
C2 pointers: supported
Drive cache: 2048 KB, caches audio, FUA works (method: fua)
Cache test (26 sectors): re-read 1.1 ms, after flush 24.5 ms, after FUA 23.8 ms
...
03 - Song.flac  CRC32 1A2B3C4D  retries 1  OK
  Cache defeat: 14 FUA command(s)
```

`--cache none` では `Drive cache: 2048 KB, method: none (disabled)`、追い出しでは `Cache defeat: 3 flush(es), 2943 sectors read` のように記録します。

注意:

- **キャッシュの挙動はドライブ依存です。** 先読みの量、バッファのうち音声に使う量、FUA の扱いはドライブごとに違い、報告されるバッファサイズが
  実際のキャッシュと一致するとも限りません。自動判定は時間の測定に基づく推定で、USB 1.1 のような遅い接続や負荷の高い環境では
  判定不能 (追い出し) になることがあります。
- 追い出しは確実ですが低速です。FUA が効くドライブなら `--cache fua` の方が速く、キャッシュを持たないと分かっているドライブなら `--cache none` で構いません。
- `cdreader drives` はキャッシュサイズのみを表示します (判定にはディスクと数秒の読み取りが必要なため、リッピング開始時に行います)。
- 実機での動作確認はこれからです。

### FLAC 出力

`--format flac` を指定すると、各トラックを FLAC (可逆圧縮、[RFC 9639](https://www.rfc-editor.org/rfc/rfc9639)) で保存します。
デコードすると元の PCM とビット単位で一致します。圧縮率は公式 `flac` コマンドの既定 (`-5`) とほぼ同等です。

- エンコーダは `core/` 内の自前実装で、外部ライブラリに依存しません。
  ブロックサイズ 4096 サンプル、サブフレームは CONSTANT / VERBATIM / FIXED (0〜4 次) / LPC (最大 8 次) から最小のものを選択、
  ステレオ相関 (L/R・L/S・S/R・M/S) も最小のものを選択、Rice 符号のパーティション分割とパラメータ探索 (エスケープ符号を含む)、wasted bits に対応。
- メタデータ: `STREAMINFO` (PCM の MD5 署名を含む)、`VORBIS_COMMENT`、`SEEKTABLE` (10 秒ごと)、`PADDING` (タグの後からの書き換え用)。
- タグ (`VORBIS_COMMENT`): `TITLE`, `ARTIST`, `ALBUM`, `ALBUMARTIST`, `TRACKNUMBER`, `TRACKTOTAL`, `DATE`, `GENRE`, `CDDB` (ディスク ID)、
  `ISRC`、`BARCODE` (MCN。読み取れた場合。MCN と ISRC の節を参照)。
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
- MCN (カタログ番号) と各トラックの ISRC も `CUESHEET` ブロックに入ります (MCN は 128 バイトの欄に 13 桁の ASCII、ISRC は 12 バイト)。
  `metaflac --import-cuesheet-from` で取り込んだ場合とバイト単位で同じブロックになることをテストで確認しています。
- プリギャップを検出したトラックは index point 0 (`INDEX 00`) と 1 (`INDEX 01`) を持ちます (トラックの開始位置は `INDEX 00`、
  `INDEX 01` はそこからの相対位置。`INDEX 02` 以降も同様)。HTOA を含むイメージではトラック 1 が 0 サンプル目の `INDEX 00` から始まります。
  これも `metaflac` で書き出し・再取り込みしてバイト単位で一致することをテストで確認しています。
- トラックごとのリッピング (`--single-file` なし) では埋め込みません (1 ファイル 1 トラックのため)。

### Ogg FLAC 出力

`--format oggflac` を指定すると、FLAC と同じ可逆圧縮のフレームを Ogg コンテナに入れた Ogg FLAC (`.oga`) で保存します
([FLAC-to-Ogg マッピング 1.0](https://xiph.org/flac/ogg_mapping.html))。Ogg に対応したプレーヤー・ツール向けで、
デコード結果は FLAC 出力と同じく元の PCM とビット単位で一致します (フレームは `--format flac` のものとバイト単位で同一)。

- エンコーダは FLAC 出力と共通 (`flac::StreamEncoder`)、Ogg のページは Ogg Opus / Vorbis と共通の `OggStreamWriter` で書きます。
- 最初のパケット: `0x7F` `FLAC`、マッピングのバージョン 1.0、ヘッダーパケット数、`fLaC`、`STREAMINFO` (MD5 署名を含む)。BOS ページにこのパケットだけを置きます。
- 続くヘッダーパケット: メタデータブロックを 1 つずつ (`VORBIS_COMMENT`、`--single-file` では `CUESHEET`)。音声は新しいページから始まります。
- 音声: 1 パケット = 1 FLAC フレーム (4096 サンプル)。グラニュール位置はそのページで完結したフレームまでのサンプル数、最後のページに EOS フラグ。
- `SEEKTABLE` は書きません (Ogg ではグラニュール位置でシークするため)。`PADDING` も書きません (Ogg のタグ編集はファイルを書き直すため)。
- `STREAMINFO` の総サンプル数・MD5・フレームサイズと `CUESHEET` のリードアウトは最後に確定するので、ファイルを閉じるときに
  ヘッダーのページ (大きさは変わりません) を作り直して先頭に上書きします (CRC も再計算)。
- タグと CUE シートの埋め込み (`CUESHEET` ブロック + `CUESHEET` タグ) は FLAC 出力と同じです
  (プリギャップの `INDEX 00`・`INDEX 02` 以降・HTOA も同じ index point になり、`CUESHEET` ブロックは FLAC 出力とバイト単位で同一)。
  `flac -t ファイル名.oga` で CRC と MD5 を検証できます。`metaflac` は Ogg FLAC を読めないため、埋め込み CUE シートは
  `flac -d --cue=2.1-3.1 ファイル名.oga` (トラック 2 だけをデコード。`--cue=2.0-2.1` ならトラック 2 のプリギャップ、
  `--cue=1.0-1.1` なら HTOA) や foobar2000 などで使えます。

### ALAC (M4A) 出力

`--format alac` を指定すると、各トラックを ALAC (Apple Lossless Audio Codec、可逆圧縮) で MP4 コンテナの `.m4a` ファイルに保存します
([#24](https://github.com/noribow/cdreader/issues/24))。デコードすると元の PCM とビット単位で一致します (FFmpeg でのデコードをテストで確認)。
iTunes / ミュージック・iPhone など Apple 製品向けの可逆圧縮形式で、それ以外の環境では FLAC を推奨します
(デコード結果は FFmpeg で確認していますが、Apple 製品での実機確認はまだ行っていません)。

- エンコーダは `core/` 内の自前実装で、外部ライブラリに依存しません (FLAC と同じく Android でもそのままビルドできます)。
  ビットストリームは Apple が公開している ALAC のリファレンス実装 ([macosforge/alac](https://github.com/macosforge/alac)、Apache License 2.0)
  の仕様に従っています。Apple のソースコードは取り込んでおらず、形式に従って書き直したものです。
  - フレームは 4096 サンプル (最後のフレームのみ短い)。マジッククッキー (`ALACSpecificConfig`) は Apple の推奨値
    (frameLength 4096、bitDepth 16、pb 40 / mb 10 / kb 14、maxRun 255、44100 Hz、2 ch) に、実際の最大フレームサイズと平均ビットレートを入れて書きます。
  - ステレオ行列 (mixBits 2、mixRes 0〜4: L/R・L/S・M/S など 5 通り) をフレームの先頭部分で比較して選択。
  - 予測器は ALAC の適応型 (符号 LMS) で、チャンネルごとに次数 0 / 4 / 8 / 16 と初期係数 (そのフレームの最小二乗解、
    または Apple のエンコーダと同じく前のフレームで適応させた 4 次の係数) の組み合わせから、実際に符号化して最小のものを選びます
    (16 次は 8 次が最小のときだけ試します)。残差は ALAC の適応ゴロム符号 (ゼロの連続はラン長で) で符号化します。
  - 圧縮しても小さくならないフレーム (白色雑音など) は非圧縮の「エスケープ」フレームで書きます。デコーダによって解釈が分かれうる
    まれなケース (予測和の 32 ビットオーバーフロー、ゼロ連続の直後の値 0xFFFF) もエスケープフレームにします。
  - 圧縮率は本プロジェクトの FLAC (`flac -5` 相当) とほぼ同等 (テスト用の音楽的な信号で FLAC より 2〜3% 大きい程度、
    純音に近い信号ではより小さい) で、FFmpeg 内蔵の ALAC エンコーダより小さくなります (`alac_ffmpeg` テストで比較を表示)。
    速度は FLAC の半分程度 (x86-64 の 1 コアで実時間の約 40 倍) です。
- MP4 の構成: `ftyp` (`M4A `、互換 `M4A ` / `mp42` / `isom`)、`mdat` (フレームを順に書き込み、サイズは最後に確定)、
  `moov` (ファイル末尾。`mvhd`、`trak` (`tkhd`、`mdia` (`mdhd` タイムスケール 44100、`hdlr` `soun`、`minf` (`smhd`、`dinf`/`dref`、
  `stbl` (`stsd` の `alac` サンプルエントリとマジッククッキー、`stts`、`stsc`、`stsz`、`stco` / `co64`))))、`udta`/`meta`/`ilst`)。
  長さ (`mvhd` / `tkhd` / `mdhd` / `stts`) はサンプル単位で正確です。4 GB を超える場合は 64 ビットのサイズ・オフセットを使います。
- タグ (iTunes 形式、UTF-8): `©nam` タイトル、`©ART` アーティスト、`©alb` アルバム、`aART` アルバムアーティスト、`trkn` トラック番号 / 総数、
  `©day` 年、`©gen` ジャンル、`©too` エンコーダ、`----:com.apple.iTunes:CDDB` (CDDB ディスク ID)、
  `----:com.apple.iTunes:ISRC` / `----:com.apple.iTunes:BARCODE` (ディスクに記録された ISRC / MCN)。
- `--single-file` も使えます (CUE シートの `FILE` の種類は `WAVE`)。CUE シートやチャプターのファイル内への埋め込みは行いません (FLAC・Ogg FLAC・MKA のみ)。
- `rip.log` の `Encoder:` 行は `ALAC (built-in encoder), lossless, MP4` です。

### 非可逆圧縮 (Opus / Vorbis)

`--format opus` / `--format vorbis` で、非可逆圧縮のファイルを作ります。
携帯プレーヤーやスマートフォン向けの「聴く用」のファイルを想定しています (保存用には FLAC を推奨)。
`--bitrate` / `--quality` を `wav` / `flac` / `oggflac` / `alac` に指定するとエラーになります。使ったコーデックと設定は `rip.log` の `Encoder:` 行に記録されます。

| 形式 | ファイル | エンコーダ | 既定 | 指定 |
| --- | --- | --- | --- | --- |
| `opus` | `.opus` (Ogg Opus、[RFC 7845](https://www.rfc-editor.org/rfc/rfc7845)) | libopus 1.5.2 | VBR 160 kbit/s | `--bitrate 6〜510` |
| `vorbis` | `.ogg` (Ogg Vorbis) | libvorbis 1.3.7 | VBR 品質 5 (約 160 kbit/s) | `--quality -1〜10` または `--bitrate 45〜500` |

- **Opus**: Opus は 48 kHz で動作するため、44.1 kHz の CD 音声を `core/` 内の自前のサンプリングレート変換
  (`resampler`: 160/147 倍のポリフェーズ FIR、カイザー窓付き sinc、通過域 0〜20 kHz、22.05 kHz 以上を 100 dB 以上減衰) で 48 kHz に変換してから
  20 ms 単位でエンコードします (libopus の `AUDIO` モード、VBR、complexity 10)。
  外部ライブラリ (speexdsp など) を使わずに済み、周波数特性・エイリアシング・長さをテストで直接検証できるため自前実装にしています。
  - `OpusHead` には pre-skip (エンコーダの先読み、通常 312 サンプル) と元のサンプリングレート (44100) を書きます。
    再生ソフトは元のレートに戻すことも、48 kHz のまま再生することもできます。
  - グラニュール位置は pre-skip を含む 48 kHz のサンプル数で、最後のページ (EOS) には「pre-skip + 変換後の正確な長さ」
    (= ⌈元のサンプル数 × 48000 / 44100⌉、opusenc と同じ) を書きます。デコーダは最後のパケットの余分な部分を捨てるため、
    `opusdec` で 48 kHz にデコードすると変換後の長さ、44.1 kHz にデコードすると元のサンプル数ちょうどに戻ります (テストで確認)。
- **Vorbis**: 44.1 kHz のまま libvorbisenc でエンコードします。`--quality` は oggenc と同じ目盛り (内部では 1/10)、
  `--bitrate` は `oggenc -b` と同じく、ビットレート管理を使わずに平均がそのビットレートになる VBR です。
  グラニュール位置と最後のパケット (EOS) は libvorbis が決め、デコード結果は元のサンプル数ちょうどになります。
- Ogg のページは Ogg FLAC と同じくコア内の `OggStreamWriter` で書きます (ヘッダーパケットはそれぞれ専用のページ、音声は新しいページから)。
- タグは FLAC と同じ Vorbis コメント (`TITLE`, `ARTIST`, `ALBUM`, `ALBUMARTIST`, `TRACKNUMBER`, `TRACKTOTAL`, `DATE`, `GENRE`, `CDDB`, `ISRC`, `BARCODE`) を
  `OpusTags` / Vorbis のコメントヘッダーに書きます。
- `--single-file` も使えます (CUE シートの `FILE` の種類は `WAVE`)。CUE シートのファイル内への埋め込みは FLAC / Ogg FLAC のみです
  (Matroska ではチャプターとして記録します)。
- 同じエンコーダで Matroska (`mka-opus` / `mka-vorbis`) にも保存できます (次節)。

### Matroska (MKA) 出力

`--format mka` で、[Matroska](https://www.rfc-editor.org/rfc/rfc9559) の音声ファイル (`.mka`) に保存します。
Matroska はコンテナなので中身のコーデックを選べます。形式名で指定します (`--codec` のような別オプションにせず、
WAV / FLAC / Opus と同じ 1 つの `--format` で選べるようにしています。Android アプリや `rip.log` でも同じ名前を使います):

| 形式 | コーデック (`CodecID`) | 内容 |
| --- | --- | --- |
| `mka` (`mka-flac` でも可) | FLAC (`A_FLAC`) | 可逆圧縮。自前の FLAC エンコーダ (`--format flac` と同じフレーム)。`CodecPrivate` は `fLaC` + `STREAMINFO` (MD5 署名・総サンプル数は閉じるときに書き込み) |
| `mka-pcm` | PCM (`A_PCM/INT/LIT`) | 無圧縮 (16 bit リトルエンディアン、100 ms ごとのブロック)。ディスクから読んだデータそのまま |
| `mka-opus` | Opus (`A_OPUS`) | `opus` と同じエンコーダ・設定 (`--bitrate`)。`CodecPrivate` = `OpusHead`、`CodecDelay` = pre-skip、`SeekPreRoll` = 80 ms |
| `mka-vorbis` | Vorbis (`A_VORBIS`) | `vorbis` と同じエンコーダ・設定 (`--quality` / `--bitrate`)。`CodecPrivate` = 3 つのヘッダー (Xiph レーシング) |

- Matroska の書き出し (EBML) は外部ライブラリを使わない自前実装 (`core/` の `matroska`) です。
  ファイル構成: EBML ヘッダー (DocType `matroska`)、Segment { `SeekHead`、`Info` (`TimestampScale` 1 ms、`Duration`、`MuxingApp` / `WritingApp`)、
  `Tracks` (音声トラック 1 本)、`Chapters` (シングルファイル時)、`Tags`、`Cluster` (最大 5 秒ごと、`SimpleBlock` はすべてキーフレーム)、`Cues` (Cluster ごと) }。
- 順に書き出しながら、最後に決まる値 (Segment のサイズ、`Duration`、`SeekHead` の `Cues` の位置、FLAC の `STREAMINFO`) は
  固定長で場所を確保しておき、閉じるときに書き換えます。サイズ不明 (unknown size) の要素は残しません。
- **終端の切り詰め**: Ogg のグラニュール位置の代わりに、最後のブロックに `DiscardPadding` (最後のパケットのうち捨てる長さ) を書きます。
  Opus は `CodecDelay` (先頭の pre-skip) と合わせて、デコード結果が 48 kHz に変換した長さちょうどになり、Vorbis は元のサンプル数ちょうどになります (ffmpeg で確認)。
- **タグ** (Matroska の `Tags`): アルバム単位 (`TargetTypeValue` 50) に `TITLE` (アルバム名)・`ARTIST` (アルバムアーティスト)・`TOTAL_PARTS` (トラック数)・
  `DATE_RELEASED`・`GENRE`・`CDDB` (ディスク ID)・`BARCODE` (MCN)、トラック単位 (30) に `TITLE`・`ARTIST`・`PART_NUMBER` (トラック番号)・`ISRC` を書きます
  (MCN / ISRC はディスクから読み取れた場合のみ。[#22](https://github.com/noribow/cdreader/issues/22))。
  階層を区別しないソフト (FFmpeg など) のために、アルバム単位には `ALBUM` (アルバム名) も書き、トラック単位のタグを後ろに置いています
  (FFmpeg / ffprobe では `title` が曲名、`album` がアルバム名、`track` がトラック番号として読めます)。
- **`--single-file`**: 外部の `.cue` に加えて、CUE シートの各トラックの位置 (`INDEX 01`) をチャプター (`ChapterAtom`、名前は曲名) として記録します。
  チャプターの時刻は 1/75 秒単位の CUE の位置をナノ秒に丸めたもので、曲名・アーティスト・トラック番号・ISRC はチャプターごとのタグ (`TagChapterUID`) にも書きます。
  チャプターは各トラックの `INDEX 01` から次のトラックの `INDEX 01` まで (最後はイメージの終わりまで) で、
  プリギャップ (`INDEX 00`) は前のトラックのチャプターの末尾に含まれます (トラックごとのリッピングでギャップを前のファイルに付ける
  のと同じ区切り。チャプターを選ぶと CUE シートのトラックと同じく `INDEX 01` から再生されます)。`INDEX 02` 以降はチャプターにしません。
  HTOA を含むイメージ ([#25](https://github.com/noribow/cdreader/issues/25)) では、先頭に 0 秒からトラック 1 の `INDEX 01` までの
  チャプター「Hidden Track」(タグの `PART_NUMBER` は 0) を加え、イメージ全体がチャプターで覆われるようにします。
- Opus / Vorbis をビルドに含めていない場合、`mka-opus` / `mka-vorbis` は使えません (「このビルドでは使えない」旨のエラーになります)。

#### MP3 / AAC に対応しない理由

- **MP3**: 実用的なエンコーダは LAME (LGPL) だけで、本プロジェクト (BSD 2-Clause) の単体実行ファイル・Android アプリに静的リンクすると
  LGPL の再リンク可能性の要件を満たすのが難しいため、組み込みません。
- **AAC**: 高品質なフリーのエンコーダは Fraunhofer FDK AAC だけですが、そのライセンスは FSF / OSI のフリーソフトウェアライセンスとして
  認められておらず (特許の扱いを含む)、再配布に問題があります。FFmpeg 内蔵のエンコーダなどは品質が劣ります。
- 同程度以上の音質・互換性は Opus (多くの環境で再生可能) や Vorbis で得られます。MP3 / AAC が必要な場合は、FLAC で保存してから
  外部のエンコーダで変換してください。Apple 製品向けには、M4A の可逆圧縮である ALAC (`--format alac`) で保存できます
  (M4A への対応 [#24](https://github.com/noribow/cdreader/issues/24) は ALAC のみで、AAC は上記の理由で対象外です)。

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

AccurateRip に登録されているディスク (よく売れた CD ほど登録が多い) を入れて `cdreader offset D:` を実行すると、
数トラックを前後に広めに読み取り、-3000〜+3000 サンプルの各オフセットで v1 / v2 チェックサムを計算してデータベースと照合します
([#37](https://github.com/noribow/cdreader/issues/37)。Android 版も同じ処理を使います)。

```
Drive: HL-DT-ST BD-RE BP71N (1.03)
Detecting the read offset with AccurateRip (offsets -3000..+3000)...
Reading track 07 (2 of at most 3)  100%

Read offset detection (AccurateRip disc id 039-0032e455-0583f8e1-fb08ee27, 5 pressing(s), offsets -3000..+3000)
  Track 12 (41 submissions): +6 v1+v2 (confidence 21, pressings 1+3), -145 v2 (confidence 13, pressing 2), -658 v2 (confidence 4, pressing 4)
  Track 07 (40 submissions): +6 v1+v2 (confidence 20, pressings 1+3), -145 v2 (confidence 13, pressing 2), -658 v2 (confidence 4, pressing 4)
  Shifted pressings, summed per offset: +6 v1+v2 confidence 41 (2 tracks, pressings 1+3), -145 v2 confidence 26 (2 tracks, pressing 2), -658 v2 confidence 8 (2 tracks, pressing 4)
  Result: Read offset +6 (2 of 2 tracks agreed, v1+v2, confidence 41; also -145 (26), -658 (8): other pressings)

Read offset: +6  (use: cdreader rip D: --offset 6; --save stores it for --offset auto)
Other pressings' offsets: -145 (confidence 26), -658 (confidence 8); fewer submissions, not used
```

(データベースに複数のプレスがあるディスクでは、一致したプレスの番号も表示します。)

- **トラックの選び方**: データベースに登録のあるオーディオトラックのうち、ディスクの最初・最後のトラック
  (大きなオフセットでディスクの外にはみ出す) を避け、長さ 10 秒〜8 分のものを登録件数 (confidence) の多い順に最大 3 トラック読みます。
  条件に合うトラックが足りなければ最初・最後のトラックや短い / 長いトラックも使います。`-t 3` / `-t 2,5` で指定もできます。
- **確定の条件**: 2 トラックが同じオフセットで一致した時点で確定します (多くの場合 2 トラックの読み取りで終わります)。
  - トラックごとに一致するオフセットが違う場合 (候補が割れた) は確定しません。
  - 一致したのが 1 トラックだけの場合、どのオフセットでも一致しない場合も確定しません。
  - **互いにずれたプレス**: よく売れた CD には、同じ音声が一定サンプル数ずれた別プレス (別の盤) の登録が混在していることがあり、
    その場合はどのトラックも複数のオフセットで一致します (例: ドライブのオフセットが +6 のとき +6 / -145 / -658)。
    読んだトラックがすべて同じオフセットの組で一致した場合 (あるオフセットがトラックで一致しないのは、そのオフセットで一致したプレスが
    そのトラックの登録を持たないときだけ許します) は、候補の割れではなくプレスの違いとみなし、オフセットごとに一致件数
    (confidence。1 トラックにつき各プレスを 1 回ずつ数え、別プレスの v1 と v2 の一致はどちらも加算) を読んだトラック全体で合計します。
    最も多いオフセットが 2 番目の **1.5 倍以上**で、かつ 2 トラック以上で一致していれば確定します
    (ドライブの本来のオフセットで読んだ人が最も多いはず、という判断です。上の例では +6 の 41 件に対し -145 は 26 件)。
    差が小さい場合は最大 3 トラックまで読んで判定し直し、それでも差がなければ「判断できない」として確定しません。
    確定した場合も、他のプレスのオフセットと件数を結果・`rip.log`・保存するメモに併記します (Android 版は「他のプレスの候補: -145, -658」)。
  - データベースに登録のあるトラックが 1 つしかないディスク (シングルなど) では、その 1 トラックの一致件数が 10 件以上のときだけ確定し、その旨を表示します。
  - ディスクが AccurateRip に登録されていない場合は「検出できない」と表示します。別の (よく売れた) CD で試してください。
- `--save` を付けると、確定したオフセットを**ドライブ (ベンダー・モデル・リビジョン) ごと**に保存します。
  `cdreader rip D: --offset auto` は保存済みの値を使い、なければその場で検出・保存してからリッピングします
  (確定できなければリッピングせずに終了します。`--offset <n>` は従来どおり手動指定です)。
- 保存先は `%APPDATA%\cdreader\drive_offsets.txt` です (環境変数 `APPDATA` がない場合は `cdreader.exe` と同じフォルダ)。
  1 行 1 ドライブのテキスト (`オフセット<TAB>ベンダー|モデル|リビジョン<TAB>メモ`、UTF-8) で、手で編集・削除してもかまいません。
  `cdreader drives` は保存済みの値も表示し、`cdreader offsets` で一覧できます。
- `--range <n>` で探索範囲 (既定 ±3000 サンプル)、`-r <n>` でリトライ回数を指定できます。
- v1 / v2 の両方で照合し、トラックごとに一致したオフセットと一致件数を表示します。
- v1 は全オフセット分を 1 回のスライド計算で求めます。v2 はサンプル値と位置の積の上位 32 ビットを含むためスライド計算できませんが、
  2^32 ≡ 1 (mod 2^32 − 1) を使った剰余のスライド計算で一致の可能性がないオフセットを除外し、残った少数のオフセットだけ厳密に計算します
  (5 分のトラック・±3000 サンプル・3 プレスで約 1 秒)。
- 検出中は読み取り中のトラックをメモリに保持します (1 サンプル 4 バイト、5 分のトラックで約 50 MB。8 分を超えるトラックはなるべく避けます)。
- `rip.log` には補正値の出所が記録されます。その場で検出した場合は、読み取ったトラックごとの結果も記録されます:

```
Read offset correction: +6 samples (auto-detected: 2 of 2 tracks agreed, v1+v2; also -145 (26), -658 (8): other pressings)
Read offset correction: +6 samples (saved for drive HL-DT-ST BD-RE BP71N (1.03); auto-detected: 2 of 2 tracks agreed, v1+v2)
Read offset correction: 0 samples (manual)
```

### タグ

トラックのメタデータ (タイトル・アーティスト・アルバム・トラック番号・年・ジャンル・CDDB ディスク ID) を WAV に書き込みます。
タイトルなどは CDDB ([#6](https://github.com/noribow/cdreader/issues/6)) で取得できた場合に入ります (取得できなければトラック番号とディスク ID のみ)。
FLAC のタグは `VORBIS_COMMENT` に、Opus / Vorbis のタグは同じ形式の Vorbis コメントに、ALAC (M4A) のタグは iTunes 形式の `ilst` に、
Matroska のタグは `Tags` 要素に書きます (FLAC 出力・ALAC 出力・非可逆圧縮・Matroska 出力の節を参照)。

- RIFF `LIST`/`INFO` チャンク: `INAM` タイトル、`IART` アーティスト、`IPRD` アルバム、`ITRK` トラック番号、`ICRD` 年、`IGNR` ジャンル、`ICMT` CDDB ディスク ID。
- `id3 ` チャンク (ID3v2.4、UTF-8): `TIT2` `TPE1` `TALB` `TPE2` `TRCK` (`番号/総数`) `TDRC` `TCON` `TSRC` (ISRC) `TXXX:DISCID` `TXXX:BARCODE` (MCN)。
  INFO を読まずに ID3 だけを読むプレイヤーが多いため両方書きます。
  ISRC と MCN は ID3 にだけ書きます (INFO の `ISRC` は「ソース (素材の提供元)」という別の意味の項目で、カタログ番号の項目もないため)。
- タグはオーディオデータ (`data` チャンク) の **後ろ** に置きます。ヘッダーは従来どおり 44 バイトの標準形のままなので、
  「サンプルは 44 バイト目から」と決め打ちする単純なツールでも読めます。RIFF ではチャンクの順序は自由で、
  ffmpeg / MediaInfo / ExifTool などは後ろのチャンクも読みます。
- INFO には文字コードの規定がありません。UTF-8 で書いており ffmpeg・MediaInfo は正しく表示しますが、
  ANSI コードページとして読むソフト (ExifTool の既定設定など) では日本語が化けます。ID3 側は UTF-8 が明示されています。

### MCN と ISRC

CD のサブチャンネル Q には、ディスクの **MCN** (Media Catalog Number。JAN / EAN-13・UPC のバーコード番号、13 桁) と
トラックごとの **ISRC** (International Standard Recording Code。`JPVI09912345` のような 12 文字の録音コード) が記録されていることがあります。
`rip` / `toc` は MMC の `READ SUB-CHANNEL` (42h、SubQ = 1、形式 02h / 03h) でこれらを読み取ります。

- `cdreader toc D:` は TOC の後に `MCN: ...` と `Track NN  ISRC: ...` を表示します。`rip` は選択したトラックの ISRC を読み、`rip.log` に同じ形で記録します。
- 記録先: CUE シートの `CATALOG` / `ISRC`、FLAC の `CUESHEET` ブロック (シングルファイル時)、タグ
  (Vorbis コメントの `ISRC` / `BARCODE`、ID3 の `TSRC` / `TXXX:BARCODE`)。シングルファイルのイメージでは MCN のみタグに入ります (ISRC はトラックごとのため CUE シート側)。
- MCN のタグ名は `BARCODE` にしています。MCN の中身は商品のバーコード番号で、MusicBrainz Picard や foobar2000 も同じ名前を使います
  (`CATALOGNUMBER` は MusicBrainz ではレーベルの品番 (`SRCL-1234` など) を指すため使いません)。
- ドライブの応答の有効ビット (MCVal / TCVal) が 0 なら「記録なし」です。MCN が全桁 0 の場合 (有効ビットを立てたまま 0 を返すドライブがあります) も記録なしとして扱います。
- 文字の検証: MCN は数字 13 桁、ISRC は国コード 2 文字 (英大文字)・登録者コード 3 文字 (英大文字または数字)・年 2 桁・番号 5 桁。
  形式に合わない値や不正な応答 (短い応答、形式コードやトラック番号の不一致) は使わず、`rip.log` に `invalid response (...)` と記録します。
- コマンドに対応していないドライブ (ILLEGAL REQUEST) では最初の 1 回で読み取りをやめます。読み取りに失敗してもリッピングは中断しません。
- ドライブによっては ISRC の読み取りに時間がかかるため、`--no-isrc` で読み取りを省略できます (`rip.log` には `MCN / ISRC: not read (disabled)`)。
- 多くの市販 CD には MCN / ISRC が記録されていません (記録されていない場合は `not present`)。

### CUE シート

リッピングのたびに、出力先に `<アーティスト> - <アルバム>.cue` (アルバム名が不明なら `CDImage.cue`) を書き出します。

- `--single-file`: 選択したトラックを 1 つのファイル (`<アーティスト> - <アルバム>.wav` / `CDImage.wav`) につなげて保存し、
  CUE シートの `INDEX 01` でファイル先頭からの各トラックの位置 (`mm:ss:ff`、ff = 1/75 秒) を示します。
  ディスク上で連続したトラックだけを指定できます (例: `-t 3-7`。`-t 1,3` はエラー)。
  オフセット補正をしても各トラックは同じだけずれた位置から切り出されるため、トラック同士は隙間・重複なくつながります
  (イメージの内容 = トラックごとにリッピングした結果を連結したもの。テストで確認しています)。
- トラックごとのリッピングでは、トラックごとに `FILE` 行を持つ CUE シートになります。
- `TITLE` / `PERFORMER`、`REM GENRE` / `REM DATE` / `REM DISCID`、TOC のコントロールビットから `FLAGS DCP` (デジタルコピー許可) / `FLAGS PRE` (プリエンファシス) を書きます。
- ディスクの MCN を `CATALOG` (ディスク全体の項目)、トラックの ISRC を `ISRC` (`TRACK` の後・`INDEX` の前。`FLAGS` の後で、`metaflac` の出力と同じ順) として書きます。
  `FILE` の種類は MP3 / AIFF 以外はすべて `WAVE` (EAC・foobar2000 と同じく、FLAC などでも `WAVE`)。
- CUE には文字列のエスケープがないため、`"` は `'` に、改行などの制御文字は空白に置き換えます。改行コードは CRLF です。
- 文字コードは UTF-8 です。ASCII だけの場合は BOM なし、日本語などを含む場合は BOM 付きにします。
  ANSI コードページ (日本語 Windows では Shift_JIS) を既定とするソフトは BOM がないと UTF-8 と判別できない一方、
  古いパーサー (cuetools 1.4 / libcue など) は BOM を 1 行目の一部として扱いその行を読み飛ばします。
  そのため 1 行目は常に読み飛ばされても困らない `REM COMMENT "cdreader"` にしています。
- プリギャップ (`INDEX 00`)・`INDEX 02` 以降・HTOA の書き方は次の節を参照してください。

### プリギャップ (INDEX 00) と HTOA の検出

TOC に載っているのは各トラックの `INDEX 01` (曲の開始位置) だけです。多くの CD ではトラックの間に
**プリギャップ** (次のトラックの `INDEX 00`。2 秒の無音が多いが、拍手やカウントなど音の入ったものもある) があり、
TOC 上は前のトラックの末尾に含まれます。また、1 曲目の `INDEX 01` が LBA 0 より後ろにあるディスクでは、その手前
(1 曲目の `INDEX 00`) に巻き戻さないと聴けない **隠しトラック (HTOA, Hidden Track One Audio)** が入っています。

`rip` / `toc` は既定でサブチャンネル Q を読んでこれらを検出します (`--no-gaps` で省略)。

- 各セクタの Q フレームには「トラック番号・インデックス番号・絶対位置」が記録されています。
  「トラック番号 × 100 + インデックス番号」はディスク上で単調に増えるため、`INDEX 01` から前へ 1, 2, 4, 8 … セクタと戻って
  (ギャロッピング) 前のトラックに入った位置を見つけ、その間を二分探索して `INDEX 00` の先頭を求めます。
  2 秒のギャップなら 1 トラックあたり 20 回程度の読み取りです (ディスク全体の上限は 4000 回。超えたトラックは「不明」)。
  `INDEX 02` 以降は各トラックの末尾 (次のプリギャップの直前) を 1 回読んで、あれば同様に探索します。
- 読み取り方法は、対応しているものを次の順に自動で選びます:
  1. `READ CD` (BEh) + サブチャンネル選択 010b (フォーマット済み Q、16 バイト/セクタ)。CRC を返すドライブでは CRC を確認
  2. `READ CD` + サブチャンネル選択 001b (生の P-W、96 バイト/セクタ)。Q をデインターリーブし、CRC-16 (CCITT、反転格納) を確認
  3. `READ CD` の後に `READ SUB-CHANNEL` (42h) 形式 01h (現在位置)。遅いため最後の手段
- 堅牢性: 1 回のコマンドで 3 セクタ読み、MCN / ISRC のフレーム (ADR 2 / 3、約 100 セクタに 1 回) や CRC エラーのセクタの代わりに隣のセクタを使います。
  位置はフレーム自身の絶対アドレスを使い、要求したセクタから 10 セクタを超えて離れたフレームは捨てます。
  見つけた境界は前後のセクタを読み直して確認し、矛盾した場合 (CRC を返さないドライブが誤ったフレームを返した場合) は
  「同じ位置で 2 回一致したフレームだけを使う」モードで探索し直します。それでも決まらないトラックは「不明」としてプリギャップなし扱いにします。
- 位置が見つからないときの再試行 (#41): 境界の近くで探索範囲が数セクタに狭まると、通常の 3 セクタの読み取りは何度やっても同じセクタになり、
  そこが ADR 2 / 3 のフレームだったり、ドライブが隣のセクタの Q を返したりすると「不明」になっていました。現在は、通常の読み取りで
  見つからない位置について、次の順に試してからあきらめます (いずれも読み取り回数の上限に数えます)。
  1. 目的の位置を中心に 16 セクタを読む (広い読み取り)、続けてその前後 16 セクタずつ (範囲に関係する場合のみ)
  2. 自動選択のときだけ、その位置に限って別の読み取り方法: フォーマット済み Q → 生の P-W → `READ SUB-CHANNEL` (現在位置)。
     対応していない方法は一度拒否されたら以後使いません
  3. どの方法でも位置の入ったフレームがない (例: 境界の先頭セクタ自体が ISRC のフレーム) 場合、未確定のセクタが 3 以下なら、
     新しいインデックスとして確認できた最初のセクタを境界とし、`INDEX 00 may start 1 sector earlier: no position in the Q frame at LBA …`
     と記録します (誤差は最大でその数のセクタ)。
- Q の遅延 (Q delay): セクタ n と一緒に n ± k の Q を返すドライブがあります。位置は常に Q フレーム自身の絶対アドレスを使うため結果はずれませんが、
  狭い範囲を読むときに目的のフレームが入らなくなります。`READ CD` で読んだ位置付きフレーム (CRC エラーのものは除く) の「フレームの位置 − 読んだセクタ」が
  8 個以上そろい 90 % 以上一致した場合はそれを遅延とみなし、読む位置をその分ずらします (`rip.log` に `Q delay: +1 sector …`)。
- 一度失敗した次のトラックのプリギャップの中で前のトラックの `INDEX 02` 以降を探し直すことはしません
  (`INDEX 02+: not searched (the pregap of track N is unknown)`。以前は同じ探索を繰り返して同じ理由で失敗していました)。
- 診断: 読んだ Q フレームを 1 回ずつ「使った」か理由別 (ADR 2/3・CRC・bad BCD・要求セクタから 10 を超えて離れた・探索範囲外・その他) に数え、
  「フレームの位置 − 読んだセクタ」の分布とあわせて `rip.log` に 1 行で記録します。「不明」になったトラックには、その位置での内訳を付けます。
- コマンドに対応していない・正しいフレームを返さないドライブでは検出をあきらめ、従来どおり `INDEX 01` だけの CUE シートになります (リッピングは続行)。
- HTOA は TOC だけで分かります (トラック 1 の開始 LBA > 0)。検出時は LBA 0 とトラック 1 の直前が「トラック 1・INDEX 00」であることを Q で確認します。
- 結果は `rip.log` と `cdreader toc` に表示します (所要時間・読み取り回数を含む):

```
Gap detection: READ CD with formatted Q sub-channel, 112 reads in 3.8 s
HTOA (hidden track before track 1): 00:32.00, LBA 0-2399, confirmed by the Q sub-channel
Track  1  pregap 00:32.00  INDEX 00 at LBA 0 (HTOA)
Track  2  pregap 00:02.00  INDEX 00 at LBA 18350
Track  3  pregap 00:00.00  INDEX 02 at LBA 40125
Track  4  pregap unknown (no usable Q frame near LBA 51230 [52 Q frames: 6 ADR 2/3, 46 out of window, position delta +1..+2; also tried raw P-W, READ SUB-CHANNEL])
Track  5  pregap 00:01.15  INDEX 00 at LBA 60327  (INDEX 00 may start 1 sector earlier: no position in the Q frame at LBA 60326)
Q frames: 402 read, 301 used; rejected: 9 ADR 2/3, 92 out of window; position delta min +1 / max +2 / median +1
Q retries: 3 positions found with a wider read, 1 with READ CD with raw P-W sub-channel
```

`Q frames` の行は読んだフレーム数・使ったフレーム数・捨てた理由の内訳・位置のずれ (最小 / 最大 / 中央値) です。
遅延が一定なら `Q delay: +1 sector (the Q frame read with sector n is that of n + 1; reads moved to make up for it)` の行が加わります。

CUE シート・FLAC への反映:

| 出力 | プリギャップ | HTOA |
| --- | --- | --- |
| `--single-file` | イメージ内の位置で `INDEX 00` / `INDEX 01` (ギャップはイメージの一部) | イメージを LBA 0 から作り、トラック 1 を `INDEX 00 00:00:00` / `INDEX 01 <HTOA の長さ>` にする (既定) |
| トラックごと | EAC の既定 (「ギャップを前のトラックに付加」、noncompliant) と同じ: ギャップは前のトラックのファイルの末尾。トラック N は前のファイルの中で `TRACK` と `INDEX 00` を書き、続けて自分の `FILE` と `INDEX 01 00:00:00` | `--htoa` でトラック 00 のファイルに保存し、トラック 1 の `INDEX 00 00:00:00` をそのファイルに置く。`--htoa` なしでは `PREGAP <長さ>` (書き込み時に同じ長さの無音を生成し、ディスクのレイアウトを保つ) |
| FLAC の `CUESHEET` (シングルファイル) | index point 0 と 1 | トラック 1 の index point 0 がファイル先頭 |

トラックごとの CUE シートの例 (トラック 2 に 2 秒のギャップ、HTOA をトラック 00 として保存):

```
FILE "00 - Hidden Track.flac" WAVE
  TRACK 01 AUDIO
    INDEX 00 00:00:00
FILE "01 - Opening.flac" WAVE
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    INDEX 00 04:11:20
FILE "02 - Song.flac" WAVE
    INDEX 01 00:00:00
```

- 位置は TOC と同じ LBA (セクタ単位) です。読み取りオフセット補正をしても、トラックの境界と同じく各インデックスの位置はそのままです。
- 一部のトラックだけを指定した場合、先頭トラックのギャップ (ファイルに含まれない) は書きません。トラックごとの場合も、直前のトラックを
  リッピングしていないトラックのギャップは書きません。
- シングルファイルではトラック 1 を含めると HTOA もイメージに入ります (`rip.log` に `Track 00 (HTOA)` の CRC32 を記録)。
  HTOA は AccurateRip の照合対象外です。トラックごとのリッピングで HTOA があり `--htoa` を付けなかった場合は、その旨を表示します。
- 検出はトラックごと・シングルファイルのどちらでも既定で行います (EAC と同様)。時間を節約したい場合は `--no-gaps` を付けます
  (HTOA は TOC から分かるため、シングルファイルではそのまま含めます)。
- ドライブによってサブチャンネルの精度が異なるため、実機での確認はこれからです ([#25](https://github.com/noribow/cdreader/issues/25))。

ドライブへのアクセスは SCSI パススルー (`IOCTL_SCSI_PASS_THROUGH_DIRECT`) を使います。
"Access is denied" になる環境では管理者として実行してください。

## 使い方 (Android)

Android 端末に USB の外付け CD/DVD ドライブを接続し、アプリから直接オーディオトラックを読み取って FLAC・Ogg FLAC・ALAC (M4A)・WAV・Opus・Vorbis・MKA (Matroska、中身は FLAC) で保存します。
Windows 版と同じコアを使うため、CDDB による曲名の取得 (タグ・ファイル名)、AccurateRip による照合、`rip.log` も同じ内容です。
root 化は不要です (Android の USB ホスト API で得たファイルディスクリプタ経由で、USB Mass Storage Bulk-Only Transport の
SCSI/MMC コマンドを送ります)。

### 必要なもの

- Android 7.0 (API 24) 以上で、**USB ホスト (OTG) に対応した端末**
- USB OTG ケーブル / アダプタ (USB Type-C 端末なら C - A 変換アダプタなど)
- **電源の足りる CD ドライブ**: 端末からの給電だけでは動かないドライブが多いため、AC アダプタ付きのドライブ、
  Y 字ケーブル、または給電機能付きの USB ハブ経由での接続を推奨します
- インターフェースクラス 8 (Mass Storage)、プロトコル 0x50 (Bulk-Only) のドライブ (一般的な USB CD/DVD ドライブ)。
  UAS 専用のドライブには未対応です
- CDDB と AccurateRip の照会にはインターネット接続が必要です (アプリは `INTERNET` 権限を使います)。
  接続できなくてもリッピング自体は行えます

### 使い方

1. ドライブに音楽 CD を入れ、端末に接続します。「cdreader を開きますか?」と表示されたら開きます
   (表示されない場合はアプリを起動して「ドライブに接続」を押し、USB へのアクセスを許可します)。
2. ドライブ名とトラック一覧 (長さ) が表示されます。「CDDB で曲名を取得」がオン (既定) なら CDDB (gnudb.org) で検索し、
   アーティスト・アルバム名・曲名を表示します。候補が複数あるときは一覧の上の選択欄で切り替えられます。
   ディスクを入れ替えたら「TOC を再読込」を押します。
3. 「保存先フォルダ」で保存先を選びます (Storage Access Framework。選んだフォルダは次回以降も使われます)。
4. 形式 (FLAC (既定) / Ogg FLAC / ALAC (M4A) / WAV / Opus / Vorbis / MKA (FLAC)。Opus は VBR 160 kbit/s、Vorbis は品質 5 の固定設定)、
   「AccurateRip で照合」(既定でオン) を設定し、保存するトラックにチェックを付けて「リッピング」を押します。
   必要に応じて「▶ 詳細設定」をタップして開き、次を設定します (開閉状態と設定は次回以降も使われます):
   - 読み取りオフセット (Windows 版の `--offset` と同じ値)。「オフセットを自動検出」を押すと、入っているディスクで
     AccurateRip を使って検出し (Windows 版の `cdreader offset` と同じ処理。進行状況を表示し、「検出を中止」で中断できます)、
     確定した値を欄に入れてドライブ (ベンダー・モデル・リビジョン) ごとに保存します。
     保存済みのドライブを接続すると自動でその値を使い、欄の横に「(ドライブ <モデル名> の保存値)」と表示します
     (欄を書き換えると手動の値になります)。
     オフセットが保存されていないドライブでオフセット 0 のままリッピングを始めると、先に自動検出するかを確認します
     (「検出する」/「このまま続ける」。接続ごとに 1 回)
   - 「2 回読みして比較 (低速)」(Windows 版の `--verify`)
   - 「C2 エラーポインタを使う (対応ドライブのみ)」(既定でオン。オフにすると Windows 版の `--no-c2` と同じ。「C2 エラーポインタ」の節を参照)
   - 「キャッシュ対策」: 自動 (既定) / FUA / 追い出し / なし (Windows 版の `--cache auto|fua|flush|none`。「ドライブキャッシュ対策」の節を参照)。
     自動ではディスクごとに最初のリッピング開始時に数秒かけてドライブを試験します
   - 「CDDB」([#38](https://github.com/noribow/cdreader/issues/38)。「CDDB 設定」の節を参照):
     「サーバー」(ドロップダウンで GNUDB (既定) / JAPDB / カスタム。カスタムのときだけ URL の入力欄を表示。[#46](https://github.com/noribow/cdreader/issues/46))、
     「連絡先メールアドレス」(gnudb は開発者/利用者の連絡先メールを要求します。入力したアドレスはサーバーに送信されます。空欄なら匿名)。
     入力内容はその場で検証し (形式が正しくない場合は欄にエラーを表示し、保存しません)、正しい値を次回以降も使います。
     「接続テスト」でサーバーに `stat` を送り、結果 (サーバーの応答またはエラー) を横に表示します。
     テストが成功し、表示中のディスクが CDDB で見つかっていなければ、新しい設定で検索し直します。
     リッピング中・検索中は変更できません。
     検索が連絡先メールアドレスの未設定で拒否された場合は、設定を促すダイアログ (「設定する」で欄を開きます) を表示します
   - 「ディスクを 1 ファイルに (CUE シート付き)」([#42](https://github.com/noribow/cdreader/issues/42)。既定でオフ。
     Windows 版の `--single-file` と同じ): 選択したトラックを 1 つのファイルにつなげ、CUE シート (`.cue`) と一緒に保存します。
     下の「シングルファイル (Android)」を参照
5. リッピング後、C2 の結果 (使ったかどうか、C2 エラーのセクタ数・再読込回数・疑わしい位置のあるトラック)、
   キャッシュ対策の結果 (判定結果と使った方法、再読込前に行った対策の回数) と
   AccurateRip の結果がトラックごとに表示されます
   (例: `Track 01: 一致 (v2) v2 12 / v1 0 / 15 件, プレス 1/2` — 一致したチェックサムの版、v2 / v1 それぞれの一致件数、
   登録件数の合計、一致したプレス数 / そのトラックの登録があるプレス数)。ディスクが AccurateRip に登録されているのに
   1 トラックも一致しなかった場合は、読み取りオフセットが合っていない可能性が高いため、オフセットの自動検出を勧めるダイアログを表示します。

保存先フォルダの下に `アーティスト - アルバム/NN - 曲名.flac` (曲名が不明なら `cd_<CDDB ID>/TrackNN.flac`。Ogg FLAC は `.oga`、ALAC は `.m4a`、Opus は `.opus`、Vorbis は `.ogg`、MKA は `.mka`) と `rip.log` が作られます。
ファイルにはタグ (曲名・アーティスト・アルバム・年・ジャンル・トラック番号・CDDB ID、ディスクに記録されていれば ISRC と MCN) が書き込まれます。
MCN / ISRC はリッピング開始時にディスクごとに 1 回読み取り、`rip.log` にも記録します。
プリギャップ・HTOA もリッピング開始時にディスクごとに 1 回検出し、`rip.log` に記録します
(トラックごとのリッピングでは記録のみで、HTOA は保存しません。シングルファイルでは CUE シートの `INDEX 00` とイメージ先頭の HTOA になります)。
`rip.log` には Windows 版と同じく、ドライブ・C2 対応状況・設定・TOC・CDDB の結果・トラックごとの CRC32 / リトライ回数 / 読めなかったセクタ数・
C2 エラーの集計と疑わしい位置・読み取りオフセットとその出所 (手動・ドライブの保存値・自動検出と一致したトラック数)・キャッシュ対策 (判定結果・試験の時間・トラックごとの回数)・AccurateRip の結果 (チェックサム v1 / v2、プレスごとの一致) が記録されます。
読み取り中は「キャンセル」で中断できます。リトライ回数は 5 回固定です。

### シングルファイル (Android)

「詳細設定」の「ディスクを 1 ファイルに (CUE シート付き)」をオンにすると、Windows 版の `--single-file` と同じく、
選択したトラックを 1 つのファイル (ディスクイメージ) にして保存します。ファイル名・CUE シート・`rip.log` の書式は Windows 版と共通のコード
(`core/include/cdreader/disc_image.h`) で作るため、同じ内容になります。

- 保存先: `アーティスト - アルバム/アーティスト - アルバム.flac` と `アーティスト - アルバム.cue`、`rip.log`
  (CDDB で見つからなければ `cd_<CDDB ID>/CDImage.flac` と `CDImage.cue`)。
- トラックはディスク上で連続している必要があります (例: 1〜12 や 3〜5。1 と 3 だけ、のような選択はエラーになります)。
  データトラックは含められません。
- CUE シートの `FILE` 行は保存したファイル名で、種類は WAV 以外も含めて `WAVE` (Windows 版と同じ慣例) です。
  MCN・ISRC・プリギャップ (`INDEX 00`)・`INDEX 02` 以降も Windows 版と同じく記録します。
- トラック 1 を含み、ディスクに HTOA (トラック 1 の前の隠しトラック) がある場合は、イメージを LBA 0 から作り、HTOA を先頭に含めます
  (`rip.log` では `Track 00 (HTOA)`)。
- CUE シートのファイル内への埋め込み:

  | 形式 | ファイル内 | 外部 `.cue` |
  | --- | --- | --- |
  | FLAC / Ogg FLAC | `CUESHEET` ブロックと `CUESHEET` タグ | あり |
  | MKA | トラックごとのチャプター | あり |
  | WAV / ALAC (M4A) / Opus / Vorbis | なし | あり |

- 読み取りオフセット補正・C2 エラーポインタ・キャッシュ対策・2 回読み比較はトラックごとのリッピングと同じです。
  AccurateRip のチェックサムはイメージの音声からトラックごとに計算するため、照合結果もトラックごとに表示します
  (HTOA は AccurateRip に登録がないため対象外)。結果の欄には保存したファイル名、HTOA を含むかどうか、イメージ全体の CRC32 も表示します。
- イメージはいったんアプリのキャッシュ領域に書き出してから保存先にコピーします (FLAC・WAV などはヘッダーを最後に書き直すため)。
  CD 1 枚分で WAV なら約 800 MB、FLAC でもその 8 割程度の空きが必要で、足りない場合は開始前にエラーになります。
- キャンセルやエラーのときは、書きかけのイメージ・`.cue` と、そのリッピングで作ったフォルダを削除します。
CDDB や AccurateRip に接続できなかった場合も、リッピングはそのまま行われます (ファイル名は `TrackNN`、結果は `rip.log` に記録)。
AccurateRip のデータベースは HTTP でしか提供されていないため、`www.accuraterip.com` に限って平文通信を許可しています
(`res/xml/network_security_config.xml`)。

## ビルド

CMake 3.18 以上と C++17 / C コンパイラが必要です。

### 依存ライブラリ (Opus / Vorbis)

Opus / Vorbis 出力用のライブラリは、CMake の構成時に `FetchContent` で GitHub から取得してビルドし、静的リンクします
(`cmake/Codecs.cmake`。git とネットワーク接続が必要です)。取得したソースはビルドディレクトリ (`_deps/`) に置かれ、リポジトリには含めません。

| CMake オプション (既定) | ライブラリ | ライセンス | 形式 |
| --- | --- | --- | --- |
| `CDREADER_WITH_OPUS` (`ON`) | [xiph/opus](https://github.com/xiph/opus) `v1.5.2` | BSD 3-Clause | `opus` |
| `CDREADER_WITH_VORBIS` (`ON`) | [xiph/ogg](https://github.com/xiph/ogg) `v1.3.5` + [xiph/vorbis](https://github.com/xiph/vorbis) `v1.3.7` | BSD 3-Clause | `vorbis` |

- オフラインでビルドする場合や不要な場合は `-DCDREADER_WITH_OPUS=OFF -DCDREADER_WITH_VORBIS=OFF` を指定します。
  その形式は `--help` の一覧に出なくなり、指定するとエラーになります (WAV / FLAC / ALAC は常に使えます)。
- libopus はそれ自身の CMake でビルドします (CPU に応じた SIMD 最適化を含む)。libogg / libvorbis は CMake 対応が古いため、
  `cmake/Codecs.cmake` でソースから直接ライブラリを定義しています。
- ライブラリのコードには本プロジェクトの警告オプション (`/W4`, `-Wall -Wextra -Wpedantic`) を適用せず、警告も表示しません。
  GCC / Clang の Debug ビルドでもライブラリは `-O2` でビルドします (テストの高速化のため)。
- 配布するバイナリ (`cdreader.exe`、Android アプリ) には上記ライブラリが含まれます。BSD 3-Clause の条件に従い、
  それぞれの著作権表示 (取得したソースの `COPYING`) を添えてください。

### Visual Studio (MSVC)

```
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

`build\Release\cdreader.exe` が生成されます (ランタイム静的リンクのため単体で動作)。

### MinGW-w64 (Linux からのクロスビルドも可)

```
cmake -S . -B build-win -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix
cmake --build build-win
```

### コアライブラリのテストのみ (Linux / macOS)

```
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

テストは仮想ドライブ (`tests/fake_drive.*`) を使い、TOC 解析・リトライ・不良セクタ処理・2 回読み比較・オフセット補正・WAV 出力・
タグ・CUE シート・Ogg ページを検証します。
C2 エラーポインタは、仮想ドライブが `MODE SENSE` ページ 2Ah (対応・非対応・コマンド自体が非対応) と、エラーフィールド付きの `READ CD`
(C2 ビットマップ) を返し、セクタごとに筋書きを指定した C2 エラー (ずっと続く・再読込で消える・C2 を報告するがデータは正しい・
毎回同じ誤データ・C2 を報告しない誤データ) と、C2 付きの読み取りを拒否する / データが足りないドライブを再現します。
CDB のバイト列、`MODE SENSE` 応答の解析、C2 ビットマップから出力セクタへの対応 (読み取りオフセットでセクタ境界・トラック境界をまたぐ場合)、
再読込の終了条件と回数 (`--verify` あり・なし、リトライ 0 回)、読み取りエラー・セクタ単位の再読込との組み合わせ、
通常の読み取りへの切り替え、疑わしい位置の書式、C2 無効・非対応時に出力とコマンド数が従来と同じであること、
C2 を報告しない誤データは `--verify` か AccurateRip でしか分からないこと、プリギャップ検出で C2 を使わないことを検証します。
ドライブキャッシュ対策 (#34) では、仮想ドライブが読み取りキャッシュ (サイズ指定、LRU、キャッシュ済みのセクタは誤っていても同じデータを高速に返す)、
`READ(12)` の FUA (有効・無視・拒否)、`MODE SENSE` のバッファサイズ、コマンドごとの擬似時間 (テストの時計として注入) を再現します。
`READ(12)` の CDB、バッファサイズの解析、追い出し量と追い出し位置の選び方 (ディスクの先頭・末尾付近、短いオーディオ領域、データトラックの後)、
ドライブの種類ごとの自動判定の結果 (キャッシュなし・FUA 有効・FUA 無視・FUA 拒否・追い出しより大きいキャッシュ・遅い接続・読み取りエラー・短いディスク)、
`--verify` の 2 回目・リトライ・セクタ単位の再読込・C2 の再読込を各方法で行ったとき、対策なしではキャッシュから一時的な誤データが
「確認」されてしまい、対策ありではディスクから読み直して正しいデータになること、`none` (とキャッシュなしと判定された場合) の
コマンド列が #33 と同じであること、途中で FUA を拒否された場合の追い出しへの切り替えを検証します。
環境変数 `CDREADER_TEST_OUTPUT` にディレクトリを指定すると、テストで作ったサンプル (タグ付き WAV、CUE、Ogg) をそこに残すので、
ffprobe / MediaInfo / ExifTool / ogginfo などの外部ツールで確認できます。
テストの一時ファイルは、テストプログラムごとにシステムの一時ディレクトリの下に作る専用のディレクトリ (`cdreader_test_<乱数>`) に書き、
終了時に削除します。そのため `ctest -j` や複数のビルドツリーのテストを同時に実行しても衝突しません
(`CDREADER_KEEP_TEST_TEMP=1` で削除せずに残します)。
FLAC は MD5 / CRC / ビット書き込み / Rice 符号の単体テストと、テスト用の簡易デコーダ (`tests/flac_decoder.*`) による往復テストに加え、
公式 `flac` コマンドがインストールされていれば (`apt-get install flac` など)、さまざまな合成信号を `flac -t` で検証・`flac -d` でデコードして
元の PCM と一致することを確認するテスト (`flac_roundtrip`) と、埋め込み CUE シート (MCN・ISRC を含む) を `metaflac` で読み出し・再取り込みして確認するテスト (`flac_cuesheet`) も実行されます (無い場合はスキップ。
`ffprobe` があれば FLAC / WAV の `ISRC` / `BARCODE` タグが読めることも確認します)。
MCN / ISRC は仮想ドライブの `READ SUB-CHANNEL` 応答で、CDB のバイト列、有効ビット 0、不正な文字・短い応答・形式コードやトラック番号の不一致、
非対応ドライブ (ILLEGAL REQUEST) を検証します。
プリギャップ・HTOA は、仮想ドライブがセクタごとのサブチャンネル Q (フォーマット済み Q・生の P-W・`READ SUB-CHANNEL` 現在位置) を返し、
Q の解析 (BCD・CRC-16・デインターリーブ)、さまざまな長さのギャップ (0・1・150・157・ほぼ 1 トラック分)、HTOA、`INDEX 02` 以降、
MCN / ISRC フレームや CRC エラー・CRC なしの誤ったフレームの混入、非対応ドライブ、読み取り回数の上限、
#41 のディスクの配置 (境界の先頭セクタが ISRC のフレーム、+2 セクタの Q 遅延。旧ロジックで「不明」になることを再現)、
-3〜+3 セクタの Q 遅延の検出と補正、境界付近の ADR 2/3 の連続、境界付近だけフォーマット済み Q が空になるドライブでの生の P-W /
`READ SUB-CHANNEL` への切り替え、診断行の書式、再試行でも読み取り回数の上限を守ること、CUE シート (シングルファイル・トラックごと)、
`CUESHEET` ブロックのバイト列、HTOA を含むイメージが各トラックの連結と一致すること (オフセット補正あり) を検証します。
HTOA・プリギャップを含むイメージは Ogg FLAC (`CUESHEET` ブロックが FLAC と同一、`flac -d --cue=1.0-1.1` / `2.0-2.1` / `3.2` で切り出し)、
MKA (チャプターの時刻・「Hidden Track」チャプター、`ffprobe` での読み取り)、ALAC (イメージの PCM がそのまま入ること) でも確認します。
Ogg FLAC は、最初のパケットのバイト列、パケット・ページ構成 (ヘッダーのページ、BOS / EOS、通し番号)、グラニュール位置、`STREAMINFO` の値を検証し、
パケットから組み立て直した FLAC を簡易デコーダでデコードして元の PCM と一致すること、フレームが FLAC 出力とバイト単位で同一であることを確かめます。
さらに `ogg_flac_check` が、インストールされているツールで `flac -t` / `flac -d` (ビット単位で一致)、埋め込み CUE シートの `CUESHEET` ブロックを使った
`flac -d --cue=` によるトラックの切り出し、`ogginfo` (警告なし)、`ffmpeg` によるデコード (ビット単位で一致)、`ffprobe` によるタグの読み取りを確認します
(どのツールも無い場合はスキップ。`metaflac` は Ogg FLAC を読めないため使いません)。
ALAC (M4A) は、マジッククッキーのバイト列、適応ゴロム符号のビット列、フレームヘッダー (部分フレーム・エスケープ・mixRes・次数)、
非圧縮フレームへのフォールバック、極端な予測係数 (int16 の折り返し) を単体テストで確認し、テスト用の MP4 リーダー・ALAC デコーダ
(`tests/alac_decoder.*`、Apple のリファレンスデコーダと同じ手順で独立に実装) で全ボックスの入れ子とサイズ、`stts` / `stsc` / `stsz` / `stco` の整合、
長さ、タグを検証して合成信号を往復させます。`ffmpeg` がインストールされていれば (`apt-get install ffmpeg` など)、すべての合成信号
(無音・正弦波・雑音・フルスケール・1〜12289 サンプルの長さ・音楽的な信号など) を `ffmpeg -i x.m4a -f s16le` でデコードして元の PCM と
ビット単位で一致すること、`ffprobe` でコーデック `alac`・サンプル数・タグ (日本語を含む) が読めることを確認し、本プロジェクトの FLAC と
FFmpeg の ALAC エンコーダとのサイズ比較を表示するテスト (`alac_ffmpeg`) が実行されます (無い場合はスキップ)。
CDDB は偽の `HttpClient` を使い (ネットワークには接続しません)、問い合わせコマンドの生成・応答コード・xmcd エントリの解析・ファイル名の変換を検証します。
CDDB 設定 (#38) は、メールアドレスの有無による hello と URL、サーバー URL・メールアドレス・アプリ名の検証、設定ファイル (`key=value`) の往復と壊れた行の無視、
接続テスト (成功・gnudb でアドレス未設定・hello の拒否・その他のエラー・通信エラー・CDDB でない応答)、
gnudb の実際の応答 `500 Unknown application, developer email for cdreader 0.1.0` からの案内文への対応付けと `rip.log` の `Hint:` 行を検証します。
サーバーのプリセット (#46) は、名前の解決 (大文字小文字)、URL からプリセットの判定 (末尾の `/`・既定のポート・スキームとホスト名の大文字小文字の正規化、
スキーム・ポート・パスが違えばカスタム)、設定の保存値 (プリセット名 / カスタム URL / 以前の URL からの移行) と設定ファイルの往復、
JAPDB の問い合わせ URL、`rip.log` の行 (`CDDB lookup (JAPDB, ...)`)、JAPDB を選んだ `RipSession` の検索 (偽の `HttpClient`) を検証します。
AccurateRip はネットワークに接続せず (偽の `HttpClient` を使用)、実在のディスクの ID・データベース応答と、独立した参照実装で求めたチェックサムで検証します。
Opus / Vorbis は、ヘッダー (`OpusHead` / `OpusTags`、Vorbis の 3 つのヘッダー) のバイト列とページ構成、
グラニュール位置 (pre-skip、最後のページでの長さの切り詰め)、EOS フラグを検証し、libopus / libvorbis のデコーダでデコードして
長さが一致すること・元の信号に近いこと (48 kHz の理論値に対する SNR。前後 1 サンプルずらすと悪化することで位置合わせも確認) を確かめます。
サンプリングレート変換は 100 Hz〜19.5 kHz の正弦波を 48 kHz の理論値と比べ (SNR 109〜125 dB)、出力の長さ・分割入力での同一性・
DC ゲイン・阻止域 (折り返し成分 -100 dB 以下) を確認します。
さらに `opus-tools` / `vorbis-tools` がインストールされていれば (`apt-get install opus-tools vorbis-tools` など)、
`opusinfo` / `ogginfo` で警告が出ないこと、`opusdec` (48 kHz / 44.1 kHz) / `oggdec` でデコードした長さが入力とちょうど一致し、
元の信号との SNR が十分であることを確認するテスト (`lossy_decode`) が実行されます。`ffprobe` があればタグ (日本語を含む) が読めることも確認します (無い場合はスキップ)。
Matroska (`cdreader_mka_tests`) は、EBML の可変長整数・各要素の符号化、小さなファイルの要素構成 (`SeekHead` / `Cues` の位置が正しい要素を指すこと、
`Duration`、Cluster の分割、サイズ不明の要素がないこと)、タグ、チャプターの時刻 = CUE の位置を独立した簡易 EBML リーダー (`tests/ebml_reader.h`) で検証し、
中身のコーデックも FLAC (簡易デコーダで入力と一致)・PCM・Opus / Vorbis (libopus / libvorbis でデコードし、`CodecDelay` / `DiscardPadding` を適用した長さが一致) を確認します。
`mkvtoolnix` / `ffmpeg` がインストールされていれば (`apt-get install mkvtoolnix ffmpeg` など)、`mkvinfo` がエラー・警告なしで読めること、
`ffmpeg` で FLAC / PCM の MKA をデコードすると入力とビット単位で一致し、Opus / Vorbis の MKA は長さがちょうど一致して SNR が十分であること、
`ffprobe` でタグとチャプター (CUE の位置) が読めることを確認するテスト (`mka_check`) も実行されます (無い場合はスキップ)。
`cdreader_usb_tests` は仮想 USB デバイス (`tests/fake_usb_device.*`) を使って Android 版の USB Bulk-Only Transport
(CBW/CSW、REQUEST SENSE、ショート転送、STALL・フェーズエラーからのリセット回復、仮想ドライブ経由のリッピング、
C2 付き読み取りの転送サイズ 24 × 2646 バイト) を検証します。
`cdreader_rip_session_tests` は Android 版のリッピング処理 (`platform/android/rip_session.*`) を仮想 USB デバイスと偽の `HttpClient` で検証します
(FLAC / Ogg FLAC / ALAC / WAV 出力とデコード結果の一致、ALAC (M4A) のタグ、Opus / Vorbis / MKA 出力のタグと `rip.log` の `Encoder:` 行、CDDB のタグ・ファイル名・フォルダ名、AccurateRip の v1 / v2 一致と件数、照会失敗時の継続、キャンセル、`rip.log`、C2 の有効・無効・非対応・切り替えと `rip.log` の C2 の行、キャッシュ対策の設定・ディスクごとに 1 回の判定・`none` でコマンドを送らないこと・FUA から追い出しへの切り替えと `rip.log` の行、USB 経由のオフセット自動検出 (オフセット 0 で不一致 → 検出 → 検出値でのリッピングで全トラック一致、互いにずれた 5 プレスの登録からの検出と `rip.log` の他プレスの候補)・照会失敗・未登録・キャンセルと `rip.log` のオフセットの出所の行、設定した CDDB サーバー・連絡先メールアドレスでの問い合わせと、hello を拒否されたときの `rip.log` の `Hint:` 行)。

読み取りオフセットの自動検出 (#37) は、仮想ドライブと偽の AccurateRip データ (各トラックを指定のオフセットで読んだチェックサム) で、正のオフセット (+6, +667)・負のオフセット (-1164, -472)・v1 / v2、トラックの選び方 (中間・登録件数・長さ・`-t`)、候補が割れる場合、互いにずれたプレス (5 プレスの登録で件数の合計による確定・件数が近いときの保留・プレスの登録がないトラック)、1 トラックだけ一致、一致なし、登録トラックが 1 つのディスク (件数による確定 / 保留)、未登録 (HTTP 404)・ネットワーク障害 (読み取りをしないこと)、キャンセルと進行状況、ディスクの最初・最後のトラックを ±3000 サンプルで端まで読むこと、保存形式の往復 (壊れた行の無視) と `rip.log` の行を検証します。

### Android アプリ

JDK 17、Android SDK (API 35)、NDK、Android SDK の CMake 3.22.1、Gradle 8.14 系が必要です
(リポジトリには Gradle Wrapper を含めていないため、Gradle は別途インストールしてください。CI は Gradle 8.14.3 を使用)。

```
cd android
echo "sdk.dir=$ANDROID_HOME" > local.properties   # ANDROID_HOME を設定済みなら不要
gradle assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

Android Studio で `android/` フォルダを開いてビルドすることもできます。
ネイティブ部分はリポジトリ直下の `CMakeLists.txt` を `externalNativeBuild` から使い、コア・USB トランスポート・
リッピング処理 (`rip_session`)・JNI を `libcdreader_jni.so` にまとめます。CDDB / AccurateRip の HTTP 通信は JNI から
Kotlin 側の `HttpGet` (`HttpURLConnection`) を呼び出して行います (ワーカースレッド上で実行)。GitHub Actions の `android` ジョブがデバッグ APK を成果物としてアップロードします。

Linux では JNI ブリッジのコンパイル確認だけを行うこともできます (要 JDK):

```
cmake -S . -B build -DCDREADER_BUILD_JNI=ON && cmake --build build
```

## 構成

```
core/       プラットフォーム非依存のコア (Windows / Android で共有)
  scsi      ScsiTransport インターフェース、センスデータ解析
  cd_drive  MMC コマンド (INQUIRY, TEST UNIT READY, READ TOC, READ CD, READ SUB-CHANNEL, MODE SENSE, READ(12) FUA)
  subchannel  サブチャンネル Q の MCN / ISRC 応答の解析・検証、ディスク全体の読み取り (DiscCodes)、セクタごとの Q フレームの解析
  gaps      プリギャップ (INDEX 00)・INDEX 02 以降・HTOA の検出 (Q の二分探索)
  toc       TOC 解析、CDDB ID
  cddb      CDDB の問い合わせ・応答解析 (HttpClient 経由)、CDDB 設定の検証・接続テスト・エラーの案内
  settings_store  設定ファイル (key=value) の読み書き
  file_naming  メタデータからのファイル名・フォルダ名・アルバム単位のファイル名 (使えない文字の置換)
  accuraterip  AccurateRip ディスク ID・チェックサム v1/v2・データベース応答の解析と照合・オフセットごとのチェックサム (スライド計算)
  offset_detect  読み取りオフセットの自動検出 (トラックの選択・一致の判定)、rip.log の出所の行、ドライブごとの保存形式
  ripper    リトライ・セクタ分割・verify・C2 エラーの再読込を含むトラック読み取り、C2 / キャッシュ対策の rip.log 行
  drive_cache  ドライブキャッシュ対策 (FUA・追い出し・自動判定)
  clock     時計のインターフェース (キャッシュ判定の時間測定。テストでは擬似時間)
  audio_writer  出力フォーマットの共通インターフェース (WAV など。コーデック・コンテナはここに追加)、エンコーダ設定
  metadata  アルバム / トラック情報 (タグ付け・ファイル名用)
  tags      RIFF INFO / ID3v2.4 / Vorbis コメントの生成
  cue_sheet CUE シートの生成
  ogg       Ogg コンテナ (ページ分割・CRC。Ogg Opus / Ogg FLAC 用、コーデック非依存)
  matroska  Matroska / EBML の書き出し (SeekHead・Cues・Tags・Chapters。コーデック非依存)
  mka_writer  Matroska 音声ファイル (FLAC / PCM / Opus / Vorbis)
  http      オンライン照会用 HTTP インターフェース (実装はプラットフォーム側)
  wav_writer, crc32
  flac_writer, flac_encoder, md5  FLAC エンコーダ (外部ライブラリなし)
  ogg_flac_writer  Ogg FLAC (flac_encoder + ogg)
  alac_writer, alac_encoder  ALAC エンコーダと M4A 出力 (外部ライブラリなし)
  mp4       MP4 コンテナのボックス・サンプルテーブル・iTunes タグの生成 (コーデック非依存)
  resampler サンプリングレート変換 (44.1 → 48 kHz、Opus 用)
  opus_writer, vorbis_writer  Ogg Opus / Ogg Vorbis (libopus / libvorbis、CMake オプションで有効時のみ)
platform/windows/   SPTI による ScsiTransport 実装、ドライブ列挙、WinHTTP クライアント
platform/android/   USB Mass Storage Bulk-Only Transport による ScsiTransport 実装 (プロトコル部分は
                    プラットフォーム非依存)、usbdevfs によるエンドポイント I/O、リッピング処理
                    (rip_session: CDDB・FLAC/Ogg FLAC/ALAC/WAV/Opus/Vorbis/MKA・AccurateRip・rip.log。JNI 非依存でテスト可能)、JNI ブリッジ
app/cli/            Windows 用コマンドラインツール
cmake/Codecs.cmake  Opus / Vorbis ライブラリの取得とビルド
android/            Android アプリ (Kotlin、Gradle)
tests/              仮想ドライブ・仮想 USB デバイスを使ったユニットテスト
```

プラットフォーム固有なのは「SCSI コマンドをドライブに送る」部分 (`ScsiTransport`) だけです。
Android 版は USB ホスト API (`UsbDeviceConnection`) のファイルディスクリプタに対して usbdevfs の ioctl
(libusb と同じ方式) でバルク転送を行い、USB Mass Storage Bulk-Only Transport で同じ MMC コマンドを送ります。
コアは NDK でビルドして共有しています。

## 今後の予定

- [ ] Android 版 (USB 外付け CD ドライブ、NDK + Kotlin UI) — [#9](https://github.com/noribow/cdreader/issues/9)
  (実装済み・実機での動作確認待ち)
- [ ] Android 版の FLAC・CDDB・AccurateRip 対応 — [#18](https://github.com/noribow/cdreader/issues/18)
  (実装済み・実機での動作確認待ち)
- [ ] Android 版の改善: 保存先へ直接書き込む (現在は一時ファイル経由でコピー)、リトライ回数の設定、UAS 専用ドライブ対応、
  CUE シート・シングルファイル出力
- [x] AccurateRip 対応 (照合、オフセット値の自動検出) — [#5](https://github.com/noribow/cdreader/issues/5)
- [ ] リードイン/リードアウトのオーバーリード
- [x] CDDB 対応 (ディスク情報の取得) — [#6](https://github.com/noribow/cdreader/issues/6)
- [ ] MusicBrainz 対応、CD-TEXT の読み取り (CDDB に無いディスクの情報取得)
- [x] オーディオコーデック対応: FLAC — [#7](https://github.com/noribow/cdreader/issues/7)
- [x] 非可逆コーデック: Opus / Vorbis — [#13](https://github.com/noribow/cdreader/issues/13)
  (MP3 / AAC はライセンス上の理由で対応しません。非可逆圧縮の節を参照)
- [ ] Android 版での Opus のビットレート / Vorbis の品質の設定 (現在は既定値固定)
- [ ] FLAC の圧縮レベル指定 (`--compression` など。現状は `flac -5` 相当の固定設定)
- [ ] コンテナフォーマット対応・タグ付け — [#8](https://github.com/noribow/cdreader/issues/8)
  - [x] WAV のタグ (INFO / ID3)、シングルファイル + CUE シート、Ogg ページ書き出し (コア)
  - [x] FLAC への CUE シート埋め込み (CUESHEET ブロック + タグ) — [#16](https://github.com/noribow/cdreader/issues/16)
  - [x] MCN / ISRC の読み取り (CUE シート・FLAC の CUESHEET・タグへの記録) — [#22](https://github.com/noribow/cdreader/issues/22)
    (実装済み・実機での動作確認待ち)
  - [x] Ogg Opus、Ogg Vorbis — [#13](https://github.com/noribow/cdreader/issues/13)
  - [x] Ogg FLAC — [#21](https://github.com/noribow/cdreader/issues/21)
  - [x] M4A (ALAC) — [#24](https://github.com/noribow/cdreader/issues/24) (AAC はライセンス上の理由で対応しません)
  - [x] MKA (Matroska。FLAC / PCM / Opus / Vorbis、タグ、シングルファイルのチャプター) — [#23](https://github.com/noribow/cdreader/issues/23)
  - [x] プリギャップ (`INDEX 00`) と HTOA の検出 (サブチャンネル Q の読み取り) — [#25](https://github.com/noribow/cdreader/issues/25)
    (実装済み・実機での動作確認待ち)
- [x] C2 エラーポインタを使った読み取りエラーの検出と再読込 (`--no-c2`、Android は詳細設定) — [#33](https://github.com/noribow/cdreader/issues/33)
  (実装済み・実機での動作確認待ち)
- [x] 再読込 (`--verify`・リトライ・C2) でのドライブキャッシュ回避 (FUA・追い出し・自動判定、`--cache`、Android は詳細設定) — [#34](https://github.com/noribow/cdreader/issues/34)
  (実装済み・実機での動作確認待ち)
- [x] 読み取りオフセットの自動検出・自動設定 (複数トラックの一致で確定、ドライブごとに保存、`rip --offset auto`・`offset --save`、
  Android は詳細設定の「オフセットを自動検出」とリッピング前の確認) — [#37](https://github.com/noribow/cdreader/issues/37)
  (実装済み・実機での動作確認待ち)
- [x] CDDB 設定 (サーバー URL・連絡先メールアドレス、接続テスト、hello を拒否されたときの案内。`cdreader config` / `cddb-test`、
  Android は詳細設定の「CDDB」) — [#38](https://github.com/noribow/cdreader/issues/38)
  (実装済み・実機での動作確認待ち)
- [x] CDDB サーバーのプリセット (GNUDB / JAPDB / カスタム。`--cddb-server japdb`、Android はドロップダウン) — [#46](https://github.com/noribow/cdreader/issues/46)
  (実装済み・実機での動作確認待ち)
- [ ] Windows GUI

## 開発の進め方

要求・機能は [GitHub issue](https://github.com/noribow/cdreader/issues) で管理し、PR には対応する issue を記載します (`Closes #番号`)。
詳しくは [CLAUDE.md](CLAUDE.md) を参照してください。

## ライセンス

[BSD 2-Clause License](LICENSE)
