# CLAUDE.md

このファイルは Claude Code がこのリポジトリで作業する際のガイドです。

## プロジェクト概要

`adpcm_packer` は、複数の WAV ファイルを ADPCM エンコードし、バウンダリ境界で
整列させたバイナリイメージ（サウンドROM用データ）を生成する CLI ツールです。
YM2608 / YM2610（レトロゲーム機で使われる FM音源+ADPCM チップ）向けの
サウンドデータ制作パイプラインの一部として使われます。

出力される `offset_hex` / `end_hex` はチップのレジスタに直接書き込む値になる
ため、フォーマットの互換性（特にバイト境界とオフセット計算）は最優先で守る
必要があります。

## ビルド

```bash
git submodule update --init --recursive   # 初回のみ: nlohmann/json取得
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

実行ファイル: `build/adpcm_packer`（Windows: `build/Release/adpcm_packer.exe`）

## テストの実行

自動テストフレームワークは導入していない。`test/` 以下の JSON を実行して
`完了。` が出力され、`[error]` が出ないことを目視確認するのが現状の方法。

```bash
for f in test/test_*.json; do
  echo "=== $f ==="
  ./build/adpcm_packer "$f"
done
```

各テストJSONの検証対象は README.md の「テスト」節、または後述の
`docs/handoff.md` を参照。出力は `output/` に生成される（gitignore対象）。

新しい機能を追加した場合は、対応する `test/test_<機能名>.json` を追加し、
既存の全テストJSONが引き続き通ることを確認してから完了とすること。

## コーディング規約・制約

- **C++17**、ヘッダオンリーで完結できるものはヘッダオンリーにする
  （`wav_reader.h`, `pitch_estimator.h` が前例）
- **外部依存はヘッダオンリーライブラリに限定**し、`extern/` にサブモジュールとして
  追加する（現状: `nlohmann/json` のみ）。新規に依存を追加する場合は同様の方針で。
- **MSVC / GCC / Clang の3系統でビルドが通ること**が必須。特に注意すべき点:
  - MSVC は `/W4 /WX` で警告をエラー扱いしている。`std::transform` +
    `::tolower` のような int→char の暗黙縮小変換に注意（過去に C4244 で
    ビルドを壊した実績あり。ラムダで `static_cast<char>` すること）
  - `-Wall -Wextra -Wpedantic -Werror` も GCC/Clang 側で有効
  - 日本語文字列を含むためMSVCビルドには `/utf-8` が必須
    （CMakeLists.txt に設定済み。削除しないこと）
- **JSONの読み書きは nlohmann/json のみを使う**。自前JSONパーサは過去に
  実装したが nlohmann/json に置き換え済み。後戻りしないこと。
- コメント・エラーメッセージ・ログ出力は日本語（ユーザーが日本語話者のため）。
- `codec.h` / `codec.cpp` はレガシー移植コード（元は Windows/MFC 環境の
  C++）。エンコードアルゴリズム自体（YmDeltaTEncoder, Ym2610AEncoder）は
  実機互換性が最重要なので、ロジックを変更しないこと。安全なリファクタ
  （Windows型の除去、初期化の明示化など）は実施済み。

## アーキテクチャ

```
src/
├── main.cpp             パラメータJSON読み込み・パッキング処理・出力JSON生成
├── codec.h / codec.cpp   ADPCMエンコーダ本体（YmDeltaTEncoder = ADPCM-B,
│                         Ym2610AEncoder = ADPCM-A）。実機互換性最優先。
├── wav_reader.h          WAV→16bitモノラルPCM取り出し（ピッチ推定・ループ検出専用。
│                         codec.cpp 内の WAV パースとは独立している）。
│                         smpl チャンク読み取り (readSmplLoop) もここ。
├── pitch_estimator.h     YINアルゴリズムによるF0推定 + MIDIノート変換 +
│                         オクターブ正規化
└── loop_detector.h       YIN周期ベースのループポイント自動検出
                          （smplチャンクが無い場合のフォールバック）
```

`wav_reader.h` と `codec.cpp` は WAV ヘッダパースのロジックが重複している
（意図的な分離）。`codec.cpp` 側はエンコード用に最適化されたリサンプリング
込みの処理、`wav_reader.h` 側はピッチ推定用の単純な PCM 取り出しのみを行う。
統合するとエンコーダに不要な結合が生まれるため、現状は分離を維持する方針。

## 現在の入力パラメータ仕様（params.json）

詳細仕様は README.md を正とする。ここでは実装上のポイントのみ記載。

- `root_note`: 整数(0-127) / ノート名文字列("C-1"〜"G9") / `"auto"`
  (YIN推定) / `"none"`・省略(デフォルト69) の4系統を受け付ける。
  パースは `main.cpp` の `parseRootNoteField` ラムダと `noteNameToMidi`
  ラムダに集約されている。
- `octave`: 整数(-1〜9) または `"none"`・省略。`normalizeToOctave()`
  （`pitch_estimator.h`）で音名を保持したままオクターブだけ変更する。
- `"auto"` 推定は信頼度閾値 `YIN_CONFIDENCE_THRESHOLD`（0.75）を下回ると
  デフォルト値69にフォールバックする。フォールバック時は stderr に警告を
  出す（既存の動作。変更する場合はREADME.mdも合わせて更新）。
- `loop`: `"none"`・省略(解析なし) / `"auto"`(smplチャンク優先→YIN自動検出
  フォールバック) / `{"start_sample":N,"end_sample":M}`(明示指定、
  リサンプリング後サンプル単位) の3系統。パースは `main.cpp` の
  `parseLoopField` ラムダ、決定ロジックは `resolveLoop()` に集約。
  `root_note` と異なり、指定が無い場合は出力JSONに loop 関連フィールドを
  一切含めない（オプトイン方式）。
  - smplチャンク読み取りは `wav_reader.h` の `readSmplLoop()`。
  - 自動検出は `loop_detector.h` の `detectLoopPoints()`。信頼度閾値
    `LOOP_CONFIDENCE_THRESHOLD`（0.5）未満は検出失敗としてフォールバック
    （非周期音・打楽器・SE等は対象外というroot_note "auto"と同じ思想）。
  - `main.cpp` の `mapSampleIndexToResampled()` で、元WAVサンプル
    インデックス（smplチャンク・自動検出とも元WAVドメインで計算）を
    リサンプリング後（エンコード対象）のサンプルインデックスへ変換する。
    この変換式は `codec.cpp` の `resampling()` の蓄積誤差アルゴリズムと
    厳密に一致させる必要がある（ズレるとADPCMニブル境界がずれてループ点
    が破綻する）。`resampling()` のロジックを変更する場合はこの関数も
    合わせて見直すこと。

## 変更時の注意

- `main.cpp` にパラメータを追加する場合は、対応箇所が3つある:
  1. `WavEntry` / `Params` 構造体へのフィールド追加
  2. `loadParams()` 内のパース・バリデーション
  3. 出力JSON生成部（`entries[]` への書き出し）
  いずれかを忘れると「読めるが出力されない」「バリデーションされない」
  という不具合になりやすいので、3箇所セットで確認すること。
- 出力JSONのフィールドは一度出したら**後方互換を壊さない**方針
  （既存フィールドの型変更・削除は避け、追加のみで対応する）。
