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
    { "path": "se1.wav", "name": "se_explosion" },  // name: 出力JSON内ラベル
    { "path": "se2.wav", "name": "se_jump" },
    "se3.wav"                                        // 文字列のみも可（name は自動生成）
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
      "name":        "se_explosion",
      "offset":      0,
      "offset_hex":  "0x000000",
      "size":        4000,
      "padded_size": 4096,
      "end_hex":     "0x000FFF"
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

`offset_hex` / `end_hex` は YM チップのスタート/エンドアドレスレジスタに
そのまま使用できます。

## プロジェクト構成

```
adpcm_packer/
├── CMakeLists.txt          # ビルド定義（本ファイル）
├── Makefile                # 簡易ビルド用（Linux/macOS のみ）
├── codec.h                 # ADPCMエンコーダ宣言
├── codec.cpp               # ADPCMエンコーダ実装
├── main.cpp                # パッカー本体
├── extern/
│   └── nlohmann_json/      # JSON ライブラリ（git submodule）
└── README.md
```

## ライセンス

本ツール固有のコードは MIT ライセンスです。  
nlohmann/json は MIT ライセンスです（`extern/nlohmann_json/LICENSE.MIT` 参照）。
