# adpcm_packer

複数の WAV ファイルを ADPCM / リニアPCM エンコードし、バウンダリ境界で
整列させたバイナリイメージを生成するツールです。  
YM2608 (ADPCM-B)、YM2610 (ADPCM-A)、YMZ280B (4bit ADPCM / 8bit・16bit
リニアPCM)、OPL4 = YMF278B (8bit・12bit・16bit リニアPCM) に対応しています。

## 必要要件

| ツール | バージョン |
|---|---|
| CMake | 3.15 以上 |
| C++ コンパイラ | C++17 対応 (MSVC 2019+, GCC 9+, Clang 10+) |
| Git | サブモジュール取得に使用 |

## クローンとサブモジュールの初期化

```bash
git clone <repository-url>
cd adpcm_packer
git submodule update --init --recursive
```

`extern/nlohmann_json/` に [nlohmann/json](https://github.com/nlohmann/json)
が展開されます。

## ビルド

### Linux / macOS (GCC / Clang)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# 実行ファイル: build/adpcm_packer
```

### Windows (MSVC / Visual Studio)

```bat
cmake -S . -B build
cmake --build build --config Release
rem 実行ファイル: build\Release\adpcm_packer.exe
```

Visual Studio の IDE からビルドする場合は、生成されたソリューションファイル
`build\adpcm_packer.sln` を開いてください。

> **Note**  
> MSVC ビルドでは `/utf-8` オプションが自動的に付与されるため、
> 日本語を含むパスやメッセージが文字化けしません。

### インストール (任意)

```bash
cmake --install build --prefix /usr/local
```

## 使い方

```bash
adpcm_packer <params.json>
```

## パラメータ JSON

```jsonc
{
  "codec":       "adpcm-b",        // "adpcm-b" / "adpcm-a" / "ymz280" / "opl4"
  "format":      "pcm8",           // "ymz280"/"opl4" のときのみ必須 (下記 format 節を参照)
  "sample_rate": 16000,            // ADPCM-B: 8000/16000/24000/32000 Hz
                                   // ADPCM-A: 指定不要（18518 Hz 固定）
                                   // ymz280/opl4: format 節の範囲内で任意の整数Hz
  "boundary":    256,              // バウンダリ境界: 32 または 256 バイト
  "output_bin":  "output.bin",     // 出力バイナリファイルパス
  "output_json": "output.json",    // 出力オフセット一覧 JSON パス
  "wav_files": [
    {
      "path":        "piano_a4.wav",
      "name":        "piano",        // 出力 JSON 内ラベル（省略時はファイル名から自動生成）
      "root_note":   "A4",           // ルートノート（省略時は "none" 扱い → 69）
      "octave":      4,              // オクターブ制約（省略時は制約なし）
      "loop":        "auto",         // ループポイント（省略時は解析しない）
      "sample_rate": 22050           // ymz280/opl4 のみ有効。このファイルだけ個別のレートで
                                     // エンコードしたい場合に指定（省略時はトップレベルの値）
    },
    "se3.wav"                      // 文字列のみの簡略記法も可
  ]
}
```

### codec / format

| `codec` | エンコーダ | 対応チップ | `format` |
|---|---|---|---|
| `adpcm-b` | YmDeltaTEncoder      | YM2608, YM2610 ADPCM-B | (不要) |
| `adpcm-a` | Ym2610AEncoder       | YM2610 ADPCM-A         | (不要) |
| `ymz280`  | Ymz280AdpcmEncoder / LinearPcm8Encoder / LinearPcm16LEEncoder | YMZ280B | `"adpcm"` / `"pcm8"` / `"pcm16"` (必須) |
| `opl4`    | LinearPcm8Encoder / Opl4Pcm12Encoder / LinearPcm16BEEncoder   | OPL4 (YMF278B)          | `"pcm8"` / `"pcm12"` / `"pcm16"` (必須) |

`ymz280`/`opl4` は `format` フィールド (string) で出力データフォーマットを
選択します（省略・不正値はエラー）。それ以外の codec で `format` を
指定した場合は警告のうえ無視されます。

- YMZ280B の 4bit ADPCM は 8bit/16bit PCM とは異なる独自の差分符号化
  （ニブル交差パッキング、上位ニブルが先）です。ADPCM-A/B とは互換性が
  ありません。
- OPL4 (YMF278B) はADPCMをサポートしません（8bit/12bit/16bitのリニア
  PCMのみ）。12bit形式は2サンプルを3バイトへパックします。
- 16bit PCMのエンディアンはチップにより異なります: YMZ280Bは
  **リトルエンディアン**、OPL4は**ビッグエンディアン**（データシート記載の
  実装に合わせています）。

### sample_rate

| codec | 指定方法 |
|---|---|
| `adpcm-b` | `8000` / `16000` / `24000` / `32000` のいずれか |
| `adpcm-a` | 指定不要（18518 Hz 固定。指定しても警告のうえ無視） |
| `ymz280` + `format:"adpcm"`  | 1〜44100 Hz の任意の整数 |
| `ymz280` + `format:"pcm8"/"pcm16"` | 1〜88200 Hz の任意の整数 |
| `opl4`（全 format） | 1〜192000 Hz の任意の整数 |

入力 WAV のサンプリング周波数は自動検出してリサンプリングします。

**per-entry 上書き（`ymz280`/`opl4` のみ）:** YMZ280B・OPL4 は実チップ側で
チャンネルごとに独立した再生レートを持てるため、`wav_files` の各エントリで
`sample_rate` を個別に指定してトップレベルの値を上書きできます
（`adpcm-a`/`adpcm-b` では上書き不可。指定しても警告のうえ無視されます）。
出力 JSON の各エントリには実際に使われた `sample_rate` が常に出力されます。

### boundary

各エントリはこの境界に切り上げてパディング（`0x00` 埋め）されます。

| 値 | 用途例 |
|---|---|
| `32`  | ADPCM-A (YM2610 は 32 byte 境界) |
| `256` | ADPCM-B (YM2608/YM2610 は 256 byte 境界)、YMZ280B、OPL4 |

### root_note

各サンプルの録音ピッチを MIDI ノート番号で指定します。
FITOM_X 側がこの値を基準に再生速度（DeltaN 等）を算出します。

| 指定値 | 型 | 動作 |
|---|---|---|
| `69` などの整数 | integer | 指定値をそのまま使用（範囲: 0〜127） |
| `"A4"` などのノート名 | string | ノート名を MIDI ノート番号に変換して使用 |
| `"auto"` | string | YIN アルゴリズムで WAV から基本周波数を推定。信頼度が低い場合はデフォルト値 69 にフォールバック |
| `"none"` または省略 | string / 省略 | デフォルト値 69 (A4) を使用 |

**ノート名の書式:** `<音名>[変音記号]<オクターブ番号>`

| 要素 | 仕様 |
|---|---|
| 音名 | `C` `D` `E` `F` `G` `A` `B`（大小文字不問） |
| 変音記号 | `#` でシャープ、`b` でフラット（省略可） |
| オクターブ番号 | `-1`〜`9` の整数 |
| 範囲 | `C-1`（MIDI 0）〜 `G9`（MIDI 127） |

```jsonc
// 指定例（すべて同じ MIDI ノート番号 69 を示す）
"root_note": 69
"root_note": "A4"
"root_note": "a4"

// 変音記号の例
"root_note": "D#4"   // → MIDI 63
"root_note": "Eb4"   // → MIDI 63（D#4 と等価）

// 自動推定
"root_note": "auto"
```

### octave

`root_note` で決定したノート番号を、指定オクターブ内の同じ音名に正規化します。
`root_note: "auto"` と組み合わせて使うことで、推定ピッチのオクターブずれを吸収できます。

| 指定値 | 動作 |
|---|---|
| `4` などの整数（`-1`〜`9`） | `C<n>`〜`B<n>` の範囲（12 音）に正規化 |
| `"none"` または省略 | 正規化しない（`C-1`〜`G9` の範囲にクランプするだけ） |

```jsonc
// 例: C5 (MIDI 72) を octave:4 で正規化 → C4 (MIDI 60)
{ "path": "piano_c5.wav", "root_note": "auto", "octave": 4 }

// 例: E4 (MIDI 64) を octave:3 で正規化 → E3 (MIDI 52)
{ "path": "piano_e4.wav", "root_note": "E4", "octave": 3 }
```

### loop

サステイン楽器音（オルガン、ストリングス等）向けのループポイントを指定します。
`loop_start_hex` / `loop_end_hex` はチップのループ制御アドレスとして、
`loop_start_sample` / `loop_end_sample` は再生タイミング計算等に使用できます。

| 指定値 | 型 | 動作 |
|---|---|---|
| `"none"` または省略 | string / 省略 | ループ解析を行わない（デフォルト。出力にループ関連フィールドは含まれない） |
| `"auto"` | string | WAV の `smpl` チャンクを優先的に読み取り、無ければ YIN アルゴリズムで自動検出する |
| `{"start_sample": N, "end_sample": M}` | object | 明示指定（リサンプリング後サンプル単位、`start_sample < end_sample`） |

**`"auto"` の挙動:**

1. WAV に `smpl` チャンク（サウンドツール側で設定済みのループ情報）があれば、それを優先して使用する。
2. 無い場合、基本周期の整数倍をループ長として、接続点（ループ終端→始端）の波形連続性
   （値・傾き）が最も良くなる区間を自動検出する。単音のサステイン楽器音が対象で、
   `root_note: "auto"` と同様に信頼度が一定未満の場合（打楽器・SE・和音など非周期音）は
   検出失敗として警告を出し、ループ情報なしにフォールバックする。

**既知の制約:** ADPCM は差分符号化のため、ループ再生時（ループ終端から始端へジャンプする
瞬間）にデコーダの内部状態（予測値・ステップサイズ）が不連続になり、微小なクリック音が
生じる場合があります。これはコーデックの性質上の制約であり、本ツールでは補正しません。

**OPL4 12bit PCM (`opl4` + `format:"pcm12"`) の制約:** 12bit形式は2サンプルを
3バイトに詰めるため、偶数番目のサンプルしかバイト境界の先頭になれません
（データシートが定める12bit開始アドレスの制約と同じ）。`loop_start_hex` /
`loop_end_hex` は奇数サンプルが指定された場合、直前の偶数サンプル境界へ
切り捨てられます。

```jsonc
// 例: smpl チャンクがあればそれを使用、無ければ自動検出
{ "path": "organ_c4.wav", "root_note": "C4", "loop": "auto" }

// 例: 自動検出結果を手動で微調整する
{ "path": "organ_c4.wav", "root_note": "C4", "loop": { "start_sample": 8820, "end_sample": 12829 } }
```

## 入力 WAV の要件

| 項目 | 対応仕様 |
|---|---|
| フォーマット | リニア PCM (`wFormatTag = 0x0001`) |
| ビット深度 | 16 bit |
| チャンネル数 | モノラル / ステレオ（ステレオは自動でモノラル化） |
| サンプリング周波数 | 任意（指定レートへ自動リサンプリング） |

## 出力 JSON の例

```json
{
  "codec": "adpcm-b",
  "sample_rate": 16000,
  "boundary": 256,
  "total_size": 8704,
  "entries": [
    {
      "name":        "piano",
      "offset":      0,
      "offset_hex":  "0x000000",
      "size":        4000,
      "padded_size": 4096,
      "end_hex":     "0x000FFF",
      "root_note":   69,
      "loop_start_sample": 8820,
      "loop_end_sample":   12829,
      "loop_start_hex":    "0x0008A4",
      "loop_end_hex":      "0x000C89",
      "loop_source":       "smpl_chunk"
    }
  ]
}
```

`codec` が `ymz280`/`opl4` の場合はトップレベルに `format` が、各エントリに
実際に使われた `sample_rate` が追加で出力されます:

```jsonc
{
  "codec": "ymz280",
  "format": "pcm8",
  "sample_rate": 22050,
  "boundary": 256,
  "total_size": 39168,
  "entries": [
    {
      "name": "override_44100",
      "offset": 11264,
      "offset_hex": "0x002C00",
      "size": 22080,
      "padded_size": 22272,
      "end_hex": "0x0082FF",
      "root_note": 69,
      "sample_rate": 44100  // このエントリだけ wav_files 側で上書きした値
    }
  ]
}
```

| フィールド | 説明 |
|---|---|
| `offset` / `offset_hex` | バイナリ内の先頭オフセット |
| `size` | エンコード後データのバイト数（パディング前） |
| `padded_size` | バウンダリ整列後のバイト数 |
| `end_hex` | パディング込み末尾アドレス |
| `root_note` | MIDI ノート番号（常に出力。省略なし） |
| `sample_rate`（エントリ側。`ymz280`/`opl4` のみ） | そのエントリで実際にエンコードに使ったサンプルレート |
| `loop_start_sample` / `loop_end_sample` | ループ範囲（リサンプリング後サンプル単位、包含。`loop` 指定時のみ出力） |
| `loop_start_hex` / `loop_end_hex` | ループ範囲のバイナリ内絶対バイトアドレス（同上） |
| `loop_source` | ループ範囲の取得元: `"smpl_chunk"` / `"auto_detected"` / `"fixed"`（同上） |

`offset_hex` / `end_hex` はチップのスタート/エンドアドレスレジスタに
そのまま使用できます。`loop_start_hex` / `loop_end_hex` も同様にループ制御用の
アドレスレジスタにそのまま使用できます（バイトあたりのサンプル数はコーデック
ごとに異なり、この変換は本ツール側で正しく行われます）。

## JSON Schema

`schema/` 配下に入力・出力それぞれの JSON Schema (Draft 7) を用意しています。

| ファイル | 対象 |
|---|---|
| `schema/params.schema.json` | 入力パラメータJSON（`adpcm_packer <params.json>` に渡すファイル） |
| `schema/output.schema.json` | 出力オフセット一覧JSON（`output_json` に生成されるファイル） |

エディタの補完・入力ミスの早期検出、CI等での自動検証に利用できます。VSCode
では params.json 側に以下を追加すると入力中に検証・補完が効きます:

```jsonc
{
  "$schema": "./schema/params.schema.json",
  "codec": "ymz280",
  ...
}
```

Python の [`jsonschema`](https://pypi.org/project/jsonschema/) 等、Draft 7 に
対応したバリデータであれば言語を問わず利用できます:

```bash
python -c "
import json, jsonschema
schema = json.load(open('schema/params.schema.json', encoding='utf-8'))
doc    = json.load(open('test/test_ymz280_pcm8.json', encoding='utf-8'))
jsonschema.validate(doc, schema)
print('OK')
"
```

`codec` が `ymz280`/`opl4` のときの `format` 必須化や、codec/format ごとの
`sample_rate` 許容範囲・`boundary` の列挙値など、README本文で説明している
バリデーションルールの主要な部分をスキーマ側にも反映しています（`loop`の
`start_sample < end_sample` など一部の相互制約はスキーマでは表現せず、
実行時のエラーメッセージに委ねています）。パラメータを追加・変更した際は
`CLAUDE.md` の「変更時の注意」に従って両スキーマも更新してください。

## プロジェクト構成

```
adpcm_packer/
├── CMakeLists.txt              # ビルド定義
├── .gitmodules
├── .gitignore
├── README.md
├── CLAUDE.md                   # Claude Code 向け作業規約
├── LICENSE
│
├── docs/                       # README 以外の補足ドキュメント
│   └── handoff.md              # 開発経緯・引継ぎ事項
│
├── schema/                     # 入力/出力 JSON の JSON Schema (Draft 7)
│   ├── params.schema.json
│   └── output.schema.json
│
├── src/                        # ソースコード
│   ├── main.cpp                # エントリポイント・パラメータ処理・パッキング
│   ├── codec.h                 # エンコーダ宣言
│   ├── codec.cpp               # エンコーダ実装 (ADPCM-B / ADPCM-A / YMZ280B / OPL4)
│   ├── wav_reader.h            # WAV → 16bit モノラル PCM 取り出しユーティリティ・smplチャンク読取
│   ├── pitch_estimator.h       # YIN アルゴリズムによる基本周波数推定
│   └── loop_detector.h         # YIN周期ベースのループポイント自動検出
│
├── extern/
│   └── nlohmann_json/          # JSON ライブラリ (git submodule)
│
└── test/                       # テスト用ファイル
    ├── test_basic_adpcm_a.json
    ├── test_basic_adpcm_b.json
    ├── test_boundary_32.json
    ├── test_loop.json
    ├── test_octave.json
    ├── test_rootnote_auto.json
    ├── test_rootnote_fixed.json
    ├── test_rootnote_notename.json
    ├── test_samplerate_8k.json / _24k.json / _32k.json
    ├── test_shorthand.json
    ├── test_ymz280_adpcm.json
    ├── test_ymz280_pcm8.json / _pcm16.json
    ├── test_ymz280_persample_rate.json
    ├── test_opl4_pcm8.json / _pcm12.json / _pcm16.json
    └── wav/
        └── *.wav
```

## ライセンス

本ツール固有のコードは MIT ライセンスです。  
nlohmann/json は MIT ライセンスです（`extern/nlohmann_json/LICENSE.MIT` 参照）。
