# adpcm_packer

複数の WAV ファイルを ADPCM エンコードし、バウンダリ境界で整列させた
バイナリイメージを生成するツールです。  
YM2608 (ADPCM-B) および YM2610 (ADPCM-A) に対応しています。

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
  "codec":       "adpcm-b",        // "adpcm-b" (YM Delta-T) または "adpcm-a" (YM2610)
  "sample_rate": 16000,            // ADPCM-B: 8000 / 16000 / 24000 / 32000 Hz
                                   // ADPCM-A: 指定不要（18518 Hz 固定）
  "boundary":    256,              // バウンダリ境界: 32 または 256 バイト
  "output_bin":  "output.bin",     // 出力バイナリファイルパス
  "output_json": "output.json",    // 出力オフセット一覧 JSON パス
  "wav_files": [
    {
      "path":      "piano_a4.wav",
      "name":      "piano",        // 出力 JSON 内ラベル（省略時はファイル名から自動生成）
      "root_note": "A4",           // ルートノート（省略時は "none" 扱い → 69）
      "octave":    4               // オクターブ制約（省略時は制約なし）
    },
    "se3.wav"                      // 文字列のみの簡略記法も可
  ]
}
```

### codec

| 値 | エンコーダ | 対応チップ |
|---|---|---|
| `adpcm-b` | YmDeltaTEncoder | YM2608, YM2610 ADPCM-B |
| `adpcm-a` | Ym2610AEncoder  | YM2610 ADPCM-A |

### sample_rate (ADPCM-B のみ)

| 値 | 備考 |
|---|---|
| `8000`  | 8 kHz |
| `16000` | 16 kHz |
| `24000` | 24 kHz |
| `32000` | 32 kHz |

入力 WAV のサンプリング周波数は自動検出してリサンプリングします。

### boundary

各エントリはこの境界に切り上げてパディング（`0x00` 埋め）されます。

| 値 | 用途例 |
|---|---|
| `32`  | ADPCM-A (YM2610 は 32 byte 境界) |
| `256` | ADPCM-B (YM2608/YM2610 は 256 byte 境界) |

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
      "root_note":   69
    }
  ]
}
```

| フィールド | 説明 |
|---|---|
| `offset` / `offset_hex` | バイナリ内の先頭オフセット |
| `size` | ADPCM データのバイト数（パディング前） |
| `padded_size` | バウンダリ整列後のバイト数 |
| `end_hex` | パディング込み末尾アドレス |
| `root_note` | MIDI ノート番号（常に出力。省略なし） |

`offset_hex` / `end_hex` は YM チップのスタート/エンドアドレスレジスタに
そのまま使用できます。

## プロジェクト構成

```
adpcm_packer/
├── CMakeLists.txt              # ビルド定義
├── .gitmodules
├── .gitignore
├── README.md
│
├── src/                        # ソースコード
│   ├── main.cpp                # エントリポイント・パラメータ処理・パッキング
│   ├── codec.h                 # ADPCM エンコーダ宣言
│   ├── codec.cpp               # ADPCM エンコーダ実装 (ADPCM-B / ADPCM-A)
│   ├── wav_reader.h            # WAV → 16bit モノラル PCM 取り出しユーティリティ
│   └── pitch_estimator.h       # YIN アルゴリズムによる基本周波数推定
│
├── extern/
│   └── nlohmann_json/          # JSON ライブラリ (git submodule)
│
└── test/                       # テスト用ファイル
    ├── test_adpcm_b.json
    ├── test_adpcm_a.json
    ├── test_pitch.json
    ├── test_notename.json
    ├── test_rootnote.json
    └── wav/
        └── *.wav
```

## ライセンス

本ツール固有のコードは MIT ライセンスです。  
nlohmann/json は MIT ライセンスです（`extern/nlohmann_json/LICENSE.MIT` 参照）。
