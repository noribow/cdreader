# cdreader

音楽 CD (CD-DA) をリッピングして WAV ファイルに保存するツールです。
対象環境は **Windows** (コマンドライン) と **Android** (USB 接続の外付け CD ドライブ) です。

> **このプロジェクトは [Claude Code](https://claude.com/claude-code) (Anthropic の AI コーディングエージェント) を利用して開発しています。**
> 設計・コード・テスト・ドキュメントの多くは Claude Code によって生成され、人がレビューしています。

## 特徴

- SCSI/MMC コマンド (`READ TOC`, `READ CD`) でドライブから直接オーディオセクタを読み取り
- 44.1 kHz / 16 bit / ステレオの WAV で保存 (トラックごとに `TrackNN.wav`)
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
```

| オプション | 説明 |
| --- | --- |
| `-o, --output <dir>` | 出力先ディレクトリ (既定: `cd_<CDDB ID>`) |
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

ドライブへのアクセスは SCSI パススルー (`IOCTL_SCSI_PASS_THROUGH_DIRECT`) を使います。
"Access is denied" になる環境では管理者として実行してください。

## 使い方 (Android)

Android 端末に USB の外付け CD/DVD ドライブを接続し、アプリから直接オーディオトラックを読み取って WAV で保存します。
root 化は不要です (Android の USB ホスト API で得たファイルディスクリプタ経由で、USB Mass Storage Bulk-Only Transport の
SCSI/MMC コマンドを送ります)。

### 必要なもの

- Android 7.0 (API 24) 以上で、**USB ホスト (OTG) に対応した端末**
- USB OTG ケーブル / アダプタ (USB Type-C 端末なら C - A 変換アダプタなど)
- **電源の足りる CD ドライブ**: 端末からの給電だけでは動かないドライブが多いため、AC アダプタ付きのドライブ、
  Y 字ケーブル、または給電機能付きの USB ハブ経由での接続を推奨します
- インターフェースクラス 8 (Mass Storage)、プロトコル 0x50 (Bulk-Only) のドライブ (一般的な USB CD/DVD ドライブ)。
  UAS 専用のドライブには未対応です

### 使い方

1. ドライブに音楽 CD を入れ、端末に接続します。「cdreader を開きますか?」と表示されたら開きます
   (表示されない場合はアプリを起動して「ドライブに接続」を押し、USB へのアクセスを許可します)。
2. ドライブ名とトラック一覧 (長さ) が表示されます。ディスクを入れ替えたら「TOC を再読込」を押します。
3. 「保存先フォルダ」で保存先を選びます (Storage Access Framework。選んだフォルダは次回以降も使われます)。
4. 必要に応じて読み取りオフセット (Windows 版の `--offset` と同じ値) と「2 回読みして比較」を設定し、
   保存するトラックにチェックを付けて「リッピング」を押します。

保存先フォルダの下に `cd_<CDDB ID>/TrackNN.wav` と `rip.log` (CRC32・リトライ回数・読めなかったセクタ数) が作られます。
読み取り中は「キャンセル」で中断できます。リトライ回数は 5 回固定です。

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
`cdreader_usb_tests` は仮想 USB デバイス (`tests/fake_usb_device.*`) を使って Android 版の USB Bulk-Only Transport
(CBW/CSW、REQUEST SENSE、ショート転送、STALL・フェーズエラーからのリセット回復、仮想ドライブ経由のリッピング) を検証します。

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
ネイティブ部分はリポジトリ直下の `CMakeLists.txt` を `externalNativeBuild` から使い、コア・USB トランスポート・JNI を
`libcdreader_jni.so` にまとめます。GitHub Actions の `android` ジョブがデバッグ APK を成果物としてアップロードします。

Linux では JNI ブリッジのコンパイル確認だけを行うこともできます (要 JDK):

```
cmake -S . -B build -DCDREADER_BUILD_JNI=ON && cmake --build build
```

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
platform/windows/   SPTI による ScsiTransport 実装、ドライブ列挙、WinHTTP クライアント
platform/android/   USB Mass Storage Bulk-Only Transport による ScsiTransport 実装 (プロトコル部分は
                    プラットフォーム非依存)、usbdevfs によるエンドポイント I/O、JNI ブリッジ
app/cli/            Windows 用コマンドラインツール
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
- [ ] Android 版の改善: 保存先へ直接書き込む (現在は一時ファイル経由でコピー)、リトライ回数の設定、UAS 専用ドライブ対応
- [ ] オフセット値の自動検出 (AccurateRip データベースとの照合)、リードイン/リードアウトのオーバーリード
- [ ] AccurateRip 対応 — [#5](https://github.com/noribow/cdreader/issues/5)
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
