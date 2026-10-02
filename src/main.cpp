// adpcm_packer  -  WAV → ADPCM バイナリパッカー
//
// 使い方:
//   adpcm_packer <params.json>

#include "codec.h"
#include "wav_reader.h"
#include "pitch_estimator.h"
#include "loop_detector.h"
#include <nlohmann/json.hpp>

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <algorithm>
#include <utility>

using json = nlohmann::json;

// ============================================================
// バイナリファイル読み込み
// ============================================================
static std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open file: " + path);
    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(f),
        std::istreambuf_iterator<char>()
    );
}

// ============================================================
// 入力パラメータ構造体
// ============================================================

// root_note の指定モード
enum class RootNoteMode {
    Fixed,  // 整数で明示指定
    Auto,   // "auto": YIN で推定
    None,   // "none" または省略: デフォルト 69
};

// loop (ループポイント) の指定モード
enum class LoopMode {
    None,   // "none" または省略: ループ解析を行わない (デフォルト)
    Auto,   // "auto": smpl チャンク読み取り → 無ければ自動検出
    Fixed,  // {"start_sample":N,"end_sample":M}: 明示指定 (リサンプリング後サンプル単位)
};

// codec + format の組み合わせを一意に解決した内部表現。
// エンコーダの選択、sample_rate の範囲チェック、出力JSONの
// 追加フィールド判定に使う。
enum class CodecKind {
    AdpcmB,       // YM2608/YM2610 ADPCM-B (YmDeltaTEncoder)
    AdpcmA,       // YM2610 ADPCM-A (Ym2610AEncoder)
    Ymz280Adpcm,  // YMZ280B 4bit ADPCM
    Ymz280Pcm8,   // YMZ280B 8bit リニアPCM
    Ymz280Pcm16,  // YMZ280B 16bit リニアPCM (リトルエンディアン)
    Opl4Pcm8,     // OPL4/YMF278B 8bit リニアPCM
    Opl4Pcm12,    // OPL4/YMF278B 12bit リニアPCM (パック)
    Opl4Pcm16,    // OPL4/YMF278B 16bit リニアPCM (ビッグエンディアン)
    Ssgs,         // YMZ705/YMZ732 (SSGS/SSGS2) 4bit ADPCM
};

// ============================================================
// SSGS (YMZ705 / YMZ732) の外部ROMレイアウト
//
// ROM 先頭は ADPCM ボイス No.0〜63 のスタート/エンドアドレステーブルで、
// 23bit アドレスを L/M/H の 3 プレーンに分けて 64 バイトずつ並べる。
// 続く曲データ用テーブルは本ツールが生成対象としないため 0 埋めする。
//
// YMZ732 は $000240〜$00047F にシンプルアクセスコード用テーブルを持ち
// データ領域が $000480 からになるが、シンプルアクセスモード非対応の前提
// (ユーザー確認済み) なので YMZ705 と共通の $000240 からボイスデータを
// 配置する。両チップはこの範囲でレジスタ・ROMマップとも互換。
// ============================================================
namespace ssgs {
constexpr uint32_t MAX_VOICES     = 64;
constexpr uint32_t DATA_AREA_BASE = 0x000240; // ボイスデータ領域の先頭
constexpr uint32_t MAX_ADDRESS    = 0x7FFFFF; // MA22〜MA0 = 8Mbyte
constexpr uint32_t TBL_START_L    = 0x000000;
constexpr uint32_t TBL_START_M    = 0x000040;
constexpr uint32_t TBL_START_H    = 0x000080;
constexpr uint32_t TBL_END_L      = 0x0000C0;
constexpr uint32_t TBL_END_M      = 0x000100;
constexpr uint32_t TBL_END_H      = 0x000140;

// チップ側が 32k/16k/8k/4kHz の4値からしか選べない
inline bool isValidSampleRate(uint32_t sr)
{
    return sr == 4000 || sr == 8000 || sr == 16000 || sr == 32000;
}

// 音指定レジスタ ($40/$50/... の D7,D6 = S1,S0) に書く値
inline int samplingCode(uint32_t sr)
{
    switch (sr) {
        case 4000:  return 0;
        case 8000:  return 1;
        case 16000: return 2;
        default:    return 3; // 32000
    }
}
} // namespace ssgs

// ============================================================
// 1サンプルの再生範囲がまたげないアドレス境界 (size == 0 は制約なし)
// ============================================================
struct SampleBank {
    uint32_t    size;
    const char* label; // メッセージ表示用
};

static SampleBank sampleBankFor(CodecKind kind)
{
    switch (kind) {
        // Y8950/YM2608 の ADPCM メモリは 256Kbit×1bit DRAM を 1〜8 個つなぐ構成で、
        // アドレス上位3bit (BANK) がチップセレクトになる。チップ境界をまたげない
        // ものとして扱う (BANK=チップセレクトからの解釈。ユーザー判断)。
        // codec を共有する YM2610 も区別せず、常にこの規則で配置する。
        case CodecKind::AdpcmB: return {0x8000,   "32KB (DRAM 1チップ分)"};
        // START/END ADDR H の D7-D4 (ROM アドレス bit23-20) は start と end で
        // 同じ値でなければならない
        case CodecKind::AdpcmA: return {0x100000, "1MB"};
        default:                return {0, ""};
    }
}

// CodecKind が per-entry sample_rate 上書きと、出力JSONへの
// sample_rate 追加フィールドの対象かどうか
static bool codecSupportsPerEntryRate(CodecKind kind)
{
    switch (kind) {
        case CodecKind::Ymz280Adpcm:
        case CodecKind::Ymz280Pcm8:
        case CodecKind::Ymz280Pcm16:
        case CodecKind::Opl4Pcm8:
        case CodecKind::Opl4Pcm12:
        case CodecKind::Opl4Pcm16:
        case CodecKind::Ssgs:
            return true;
        default:
            return false;
    }
}

// CodecKind が params/出力JSON の 'format' フィールドを持つかどうか
static bool codecHasFormat(CodecKind kind)
{
    switch (kind) {
        case CodecKind::Ymz280Adpcm:
        case CodecKind::Ymz280Pcm8:
        case CodecKind::Ymz280Pcm16:
        case CodecKind::Opl4Pcm8:
        case CodecKind::Opl4Pcm12:
        case CodecKind::Opl4Pcm16:
            return true;
        default:
            return false;
    }
}

struct WavEntry {
    std::string  path;
    std::string  name;
    RootNoteMode rootNoteMode = RootNoteMode::None;
    int          rootNoteFixed = 69; // mode == Fixed のときのみ有効
    int          octave = -1;        // -1 = 制約なし ("none" または省略)
    LoopMode     loopMode = LoopMode::None;
    uint32_t     loopFixedStart = 0; // mode == Fixed のときのみ有効
    uint32_t     loopFixedEnd   = 0; // mode == Fixed のときのみ有効
    bool         hasSampleRateOverride = false; // ymz280/opl4 のみ有効
    uint32_t     sampleRateOverride    = 0;
};

struct Params {
    std::string           codec;
    CodecKind              kind = CodecKind::AdpcmB;
    std::string           format;      // ymz280/opl4 のときのみ有効な値を保持
    uint32_t              sampleRate = 0;
    uint32_t              boundary   = 0;
    uint32_t              memorySize = 0; // 0 = 未指定 (上限チェックなし)
    std::string           outputBin;
    std::string           outputJson;
    std::vector<WavEntry> wavFiles;
};

// ============================================================
// 文字列の小文字化ヘルパー
// ============================================================
static std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ============================================================
// パラメータ JSON 読み込み  (nlohmann/json 使用)
// ============================================================
static Params loadParams(const std::string& jsonPath)
{
    std::ifstream f(jsonPath);
    if (!f) throw std::runtime_error("Cannot open params file: " + jsonPath);

    json root;
    try {
        root = json::parse(f, nullptr, /*exceptions=*/true, /*ignore_comments=*/true);
    } catch (const json::parse_error& e) {
        throw std::runtime_error(std::string("JSON parse error: ") + e.what());
    }

    Params p;

    // --- codec ---
    if (!root.contains("codec") || !root["codec"].is_string())
        throw std::runtime_error("Missing or invalid 'codec' field (string required)");
    p.codec = toLower(root["codec"].get<std::string>());
    if (p.codec != "adpcm-b" && p.codec != "adpcm-a" &&
        p.codec != "ymz280"  && p.codec != "opl4" && p.codec != "ssgs")
        throw std::runtime_error("'codec' must be 'adpcm-b' / 'adpcm-a' / 'ymz280' / 'opl4' / 'ssgs'");

    // --- format (ymz280 / opl4 のみ必須) ---
    if (p.codec == "ymz280" || p.codec == "opl4") {
        if (!root.contains("format") || !root["format"].is_string())
            throw std::runtime_error(
                "codec='" + p.codec + "' には 'format' フィールド (string) が必須です");
        p.format = toLower(root["format"].get<std::string>());

        if (p.codec == "ymz280") {
            if      (p.format == "adpcm") p.kind = CodecKind::Ymz280Adpcm;
            else if (p.format == "pcm8")  p.kind = CodecKind::Ymz280Pcm8;
            else if (p.format == "pcm16") p.kind = CodecKind::Ymz280Pcm16;
            else throw std::runtime_error(
                "codec='ymz280' の format は 'adpcm'/'pcm8'/'pcm16' のいずれかです");
        } else { // opl4
            if      (p.format == "pcm8")  p.kind = CodecKind::Opl4Pcm8;
            else if (p.format == "pcm12") p.kind = CodecKind::Opl4Pcm12;
            else if (p.format == "pcm16") p.kind = CodecKind::Opl4Pcm16;
            else throw std::runtime_error(
                "codec='opl4' の format は 'pcm8'/'pcm12'/'pcm16' のいずれかです");
        }
    } else if (root.contains("format")) {
        std::cerr << "[warn] codec='" << p.codec << "' では 'format' は使用されません。指定値は無視されます。\n";
    }
    if (p.codec == "adpcm-b") p.kind = CodecKind::AdpcmB;
    if (p.codec == "adpcm-a") p.kind = CodecKind::AdpcmA;
    if (p.codec == "ssgs")    p.kind = CodecKind::Ssgs;

    // --- sample_rate ---
    // codec/format ごとの許容範囲。ADPCM-A は固定レートのため範囲チェック対象外。
    auto sampleRateRangeFor = [](CodecKind kind) -> std::pair<uint32_t, uint32_t> {
        switch (kind) {
            case CodecKind::Ymz280Adpcm:               return {1, 44100};
            case CodecKind::Ymz280Pcm8:
            case CodecKind::Ymz280Pcm16:               return {1, 88200};
            case CodecKind::Opl4Pcm8:
            case CodecKind::Opl4Pcm12:
            case CodecKind::Opl4Pcm16:                 return {1, 192000};
            default:                                   return {1, 0xFFFFFFFFu};
        }
    };

    if (p.codec == "adpcm-a") {
        p.sampleRate = 18518;
        if (root.contains("sample_rate"))
            std::cerr << "[warn] ADPCM-A の sample_rate は 18518 Hz 固定です。指定値は無視されます。\n";
    } else if (p.codec == "adpcm-b") {
        if (!root.contains("sample_rate") || !root["sample_rate"].is_number_integer())
            throw std::runtime_error("Missing or invalid 'sample_rate' field (integer required)");
        uint32_t sr = root["sample_rate"].get<uint32_t>();
        if (sr != 8000 && sr != 16000 && sr != 24000 && sr != 32000)
            throw std::runtime_error("ADPCM-B の sample_rate は 8000/16000/24000/32000 Hz のいずれかです");
        p.sampleRate = sr;
    } else if (p.codec == "ssgs") {
        if (!root.contains("sample_rate") || !root["sample_rate"].is_number_integer())
            throw std::runtime_error("Missing or invalid 'sample_rate' field (integer required)");
        uint32_t sr = root["sample_rate"].get<uint32_t>();
        if (!ssgs::isValidSampleRate(sr))
            throw std::runtime_error("SSGS の sample_rate は 4000/8000/16000/32000 Hz のいずれかです");
        p.sampleRate = sr;
    } else {
        // ymz280 / opl4: 任意の整数値。codec/format ごとの範囲でのみ検証する。
        if (!root.contains("sample_rate") || !root["sample_rate"].is_number_integer())
            throw std::runtime_error("Missing or invalid 'sample_rate' field (integer required)");
        uint32_t sr = root["sample_rate"].get<uint32_t>();
        auto [lo, hi] = sampleRateRangeFor(p.kind);
        if (sr < lo || sr > hi)
            throw std::runtime_error(
                "'sample_rate' は " + std::to_string(lo) + "〜" + std::to_string(hi)
                + " Hz の範囲で指定してください (codec='" + p.codec + "', format='" + p.format + "')");
        p.sampleRate = sr;
    }

    // --- boundary ---
    if (!root.contains("boundary") || !root["boundary"].is_number_integer())
        throw std::runtime_error("Missing or invalid 'boundary' field (integer required)");
    {
        uint32_t b = root["boundary"].get<uint32_t>();
        if (b != 32 && b != 256)
            throw std::runtime_error("'boundary' は 32 または 256 を指定してください");
        p.boundary = b;
    }

    // --- memory_size (省略可) ---
    if (root.contains("memory_size")) {
        const auto& m = root["memory_size"];
        if (!m.is_number_unsigned() || m.get<uint64_t>() == 0 || m.get<uint64_t>() > 0xFFFFFFFFu)
            throw std::runtime_error("'memory_size' は 1〜4294967295 の整数 (バイト数) で指定してください");
        p.memorySize = m.get<uint32_t>();
    }

    // --- output_bin ---
    if (!root.contains("output_bin") || !root["output_bin"].is_string())
        throw std::runtime_error("Missing or invalid 'output_bin' field (string required)");
    p.outputBin = root["output_bin"].get<std::string>();

    // --- output_json ---
    if (!root.contains("output_json") || !root["output_json"].is_string())
        throw std::runtime_error("Missing or invalid 'output_json' field (string required)");
    p.outputJson = root["output_json"].get<std::string>();

    // --- wav_files ---
    if (!root.contains("wav_files") || !root["wav_files"].is_array())
        throw std::runtime_error("Missing or invalid 'wav_files' field (array required)");

    // ファイルパスから name を自動生成
    auto autoName = [](const std::string& path) -> std::string {
        auto sep = path.find_last_of("/\\");
        std::string name = (sep == std::string::npos) ? path : path.substr(sep + 1);
        if (name.size() > 4 && name.substr(name.size() - 4) == ".wav")
            name = name.substr(0, name.size() - 4);
        return name;
    };

    // ノート名文字列 ("C4", "A#3", "Bb-1" 等) を MIDI ノート番号に変換する。
    // 成功時は 0〜127 の値を返す。パース失敗時は -1 を返す。
    //
    // 対応書式: <音名>[#/b/♯/♭]<オクターブ番号>
    //   音名   : C D E F G A B (大小文字不問)
    //   変音記号: # ♯ → 半音上、b ♭ → 半音下 (省略可)
    //   オクターブ: -1〜9 の整数
    //   例: C4, A#3, Bb-1, G9, c-1, f#5, Eb2
    auto noteNameToMidi = [](const std::string& raw) -> int {
        if (raw.empty()) return -1;

        size_t i = 0;

        // 音名
        static const int semitone[] = {9,11,0,2,4,5,7}; // A B C D E F G
        char noteCh = static_cast<char>(std::toupper(static_cast<unsigned char>(raw[i])));
        if (noteCh < 'A' || noteCh > 'G') return -1;
        int note = semitone[noteCh - 'A'];
        ++i;

        // 変音記号
        int accidental = 0;
        if (i < raw.size()) {
            char c = raw[i];
            if (c == '#' || c == static_cast<char>(0xe2)) { // # or UTF-8 ♯(e2 99 af)
                accidental = +1; ++i;
                // UTF-8 ♯ は 3バイト (e2 99 af) — 残り2バイトをスキップ
                if (c == static_cast<char>(0xe2) && i + 1 < raw.size()) i += 2;
            } else if (c == 'b' || c == 'B') {
                // 'b' はノート名 B と衝突するが、ここに来た時点で音名は確定済みなので
                // 変音記号として扱う。ただし次の文字が数字や '-' でない場合は不正
                size_t next = i + 1;
                if (next < raw.size() && (std::isdigit(static_cast<unsigned char>(raw[next]))
                                          || raw[next] == '-')) {
                    accidental = -1; ++i;
                }
            } else if (c == static_cast<char>(0xe2)) {
                // UTF-8 ♭ (e2 99 ad) — 先頭バイトが同じなのでここは到達しないが念のため
                accidental = -1; i += 3;
            }
        }

        // オクターブ番号
        if (i >= raw.size()) return -1;
        // stoi で残りを丸ごとパース（"-1" 対応）
        try {
            size_t consumed = 0;
            int octave = std::stoi(raw.substr(i), &consumed);
            if (i + consumed != raw.size()) return -1; // 末尾に余分な文字
            if (octave < -1 || octave > 9) return -1;
            int midi = (octave + 1) * 12 + note + accidental;
            if (midi < 0 || midi > 127) return -1;
            return midi;
        } catch (...) {
            return -1;
        }
    };

    // root_note フィールドのパース
    // 文字列 "auto"/"none"/ノート名 または整数 0〜127 を受け付ける
    auto parseRootNoteField = [&noteNameToMidi](const json& item, const std::string& entryPath,
                                  RootNoteMode& outMode, int& outFixed)
    {
        if (!item.contains("root_note")) {
            outMode  = RootNoteMode::None;
            outFixed = 69;
            return;
        }
        const auto& rn = item["root_note"];
        if (rn.is_string()) {
            std::string s = rn.get<std::string>();
            std::string sl = toLower(s);
            if (sl == "auto") {
                outMode  = RootNoteMode::Auto;
                outFixed = 69;
            } else if (sl == "none") {
                outMode  = RootNoteMode::None;
                outFixed = 69;
            } else {
                // ノート名として解釈を試みる
                int midi = noteNameToMidi(s);
                if (midi < 0)
                    throw std::runtime_error(
                        "'" + entryPath + "' の root_note に無効な値です: \"" + s + "\""
                        " (整数 0〜127、ノート名 C-1〜G9、\"auto\"、\"none\" のいずれかを指定)");
                outMode  = RootNoteMode::Fixed;
                outFixed = midi;
            }
        } else if (rn.is_number_integer()) {
            int v = rn.get<int>();
            if (v < 0 || v > 127)
                throw std::runtime_error(
                    "'" + entryPath + "' の root_note は 0〜127 の範囲で指定してください (指定値: "
                    + std::to_string(v) + ")");
            outMode  = RootNoteMode::Fixed;
            outFixed = v;
        } else {
            throw std::runtime_error(
                "'" + entryPath + "' の root_note は整数または \"auto\"/\"none\" を指定してください");
        }
    };

    // octave フィールドのパース
    // 整数 0〜9 または "none" を受け付ける
    auto parseOctaveField = [](const json& item, const std::string& entryPath) -> int
    {
        if (!item.contains("octave")) return -1; // 省略 = 制約なし
        const auto& oc = item["octave"];
        if (oc.is_string()) {
            if (toLower(oc.get<std::string>()) == "none") return -1;
            throw std::runtime_error(
                "'" + entryPath + "' の octave に無効な文字列です (整数または \"none\" を指定)");
        }
        if (!oc.is_number_integer())
            throw std::runtime_error(
                "'" + entryPath + "' の octave は整数または \"none\" を指定してください");
        int v = oc.get<int>();
        // C-1(octave=-1, MIDI 0)〜G9(octave=9, MIDI 127) の範囲で意味を持つ
        // ユーザー指定として妥当な範囲は -1〜9
        if (v < -1 || v > 9)
            throw std::runtime_error(
                "'" + entryPath + "' の octave は -1〜9 の範囲で指定してください (指定値: "
                + std::to_string(v) + ")");
        return v;
    };

    // loop フィールドのパース
    // 省略/"none" (解析しない) / "auto" (smplチャンク→自動検出) /
    // {"start_sample":N,"end_sample":M} (明示指定) を受け付ける
    auto parseLoopField = [](const json& item, const std::string& entryPath,
                              LoopMode& outMode, uint32_t& outStart, uint32_t& outEnd)
    {
        outMode  = LoopMode::None;
        outStart = 0;
        outEnd   = 0;
        if (!item.contains("loop")) return;

        const auto& lp = item["loop"];
        if (lp.is_string()) {
            std::string s = toLower(lp.get<std::string>());
            if (s == "none") {
                outMode = LoopMode::None;
            } else if (s == "auto") {
                outMode = LoopMode::Auto;
            } else {
                throw std::runtime_error(
                    "'" + entryPath + "' の loop に無効な文字列です: \"" + s + "\""
                    " (\"auto\"、\"none\"、または {start_sample,end_sample} オブジェクトを指定)");
            }
        } else if (lp.is_object()) {
            if (!lp.contains("start_sample") || !lp["start_sample"].is_number_integer() ||
                !lp.contains("end_sample")   || !lp["end_sample"].is_number_integer())
                throw std::runtime_error(
                    "'" + entryPath + "' の loop オブジェクトには "
                    "start_sample / end_sample (整数) が必要です");
            int64_t startV = lp["start_sample"].get<int64_t>();
            int64_t endV   = lp["end_sample"].get<int64_t>();
            if (startV < 0 || endV < 0 || endV <= startV)
                throw std::runtime_error(
                    "'" + entryPath + "' の loop は 0 <= start_sample < end_sample を満たす必要があります");
            outMode  = LoopMode::Fixed;
            outStart = static_cast<uint32_t>(startV);
            outEnd   = static_cast<uint32_t>(endV);
        } else {
            throw std::runtime_error(
                "'" + entryPath + "' の loop は \"auto\"/\"none\" または "
                "{start_sample,end_sample} オブジェクトを指定してください");
        }
    };

    // sample_rate フィールドのパース (wav_files の各オブジェクト要素向け)
    // ymz280/opl4 のみ有効。それ以外の codec で指定された場合は警告のうえ無視する
    // (ADPCM-A の固定レート警告と同じトーン)。
    auto parseSampleRateField = [&p, &sampleRateRangeFor](const json& item, const std::string& entryPath,
                                    bool& outHasOverride, uint32_t& outOverride)
    {
        outHasOverride = false;
        outOverride    = 0;
        if (!item.contains("sample_rate")) return;

        if (!codecSupportsPerEntryRate(p.kind)) {
            std::cerr << "[warn] '" << entryPath << "' の sample_rate はcodec='" << p.codec
                      << "' では使用されません。指定値は無視されます。\n";
            return;
        }
        if (!item["sample_rate"].is_number_integer())
            throw std::runtime_error(
                "'" + entryPath + "' の sample_rate は整数で指定してください");
        uint32_t sr = item["sample_rate"].get<uint32_t>();
        if (p.kind == CodecKind::Ssgs) {
            if (!ssgs::isValidSampleRate(sr))
                throw std::runtime_error(
                    "'" + entryPath + "' の sample_rate は 4000/8000/16000/32000 Hz のいずれかです");
        } else {
            auto [lo, hi] = sampleRateRangeFor(p.kind);
            if (sr < lo || sr > hi)
                throw std::runtime_error(
                    "'" + entryPath + "' の sample_rate は " + std::to_string(lo) + "〜"
                    + std::to_string(hi) + " Hz の範囲で指定してください");
        }
        outHasOverride = true;
        outOverride    = sr;
    };

    for (auto& item : root["wav_files"]) {
        WavEntry entry;
        if (item.is_string()) {
            // 文字列簡略記法: root_note=none 扱い、octave=制約なし
            entry.path          = item.get<std::string>();
            entry.name          = autoName(entry.path);
            entry.rootNoteMode  = RootNoteMode::None;
            entry.rootNoteFixed = 69;
            entry.octave        = -1;
            entry.loopMode      = LoopMode::None;
        } else if (item.is_object()) {
            if (!item.contains("path") || !item["path"].is_string())
                throw std::runtime_error("wav_files の各オブジェクト要素に 'path' (string) が必要です");
            entry.path = item["path"].get<std::string>();
            entry.name = item.contains("name") && item["name"].is_string()
                       ? item["name"].get<std::string>()
                       : autoName(entry.path);
            parseRootNoteField(item, entry.path, entry.rootNoteMode, entry.rootNoteFixed);
            entry.octave = parseOctaveField(item, entry.path);
            parseLoopField(item, entry.path, entry.loopMode, entry.loopFixedStart, entry.loopFixedEnd);
            parseSampleRateField(item, entry.path, entry.hasSampleRateOverride, entry.sampleRateOverride);
        } else {
            throw std::runtime_error("wav_files の各要素はパス文字列またはオブジェクトである必要があります");
        }
        p.wavFiles.push_back(std::move(entry));
    }

    if (p.wavFiles.empty())
        throw std::runtime_error("'wav_files' が空です");

    if (p.kind == CodecKind::Ssgs && p.wavFiles.size() > ssgs::MAX_VOICES)
        throw std::runtime_error(
            "SSGS の ADPCM ボイスは最大 " + std::to_string(ssgs::MAX_VOICES)
            + " 音です (指定: " + std::to_string(p.wavFiles.size()) + " 音)");

    return p;
}

// ============================================================
// バウンダリ整列
// ============================================================
static uint32_t alignUp(uint32_t value, uint32_t boundary)
{
    uint32_t rem = value % boundary;
    return (rem == 0) ? value : value + (boundary - rem);
}

// ============================================================
// SSGS ボイスアドレステーブルへの書き込み
// 23bit アドレスを L(bit7-0) / M(bit15-8) / H(bit22-16) の
// 3プレーンに分割して、ボイス番号をインデックスに格納する。
// ============================================================
static void writeSsgsVoiceTable(std::vector<uint8_t>& bin, uint32_t voiceNo,
                                uint32_t startAddr, uint32_t endAddr)
{
    bin[ssgs::TBL_START_L + voiceNo] = static_cast<uint8_t>( startAddr        & 0xFF);
    bin[ssgs::TBL_START_M + voiceNo] = static_cast<uint8_t>((startAddr >>  8) & 0xFF);
    bin[ssgs::TBL_START_H + voiceNo] = static_cast<uint8_t>((startAddr >> 16) & 0x7F);
    bin[ssgs::TBL_END_L   + voiceNo] = static_cast<uint8_t>( endAddr          & 0xFF);
    bin[ssgs::TBL_END_M   + voiceNo] = static_cast<uint8_t>((endAddr   >>  8) & 0xFF);
    bin[ssgs::TBL_END_H   + voiceNo] = static_cast<uint8_t>((endAddr   >> 16) & 0x7F);
}

// ============================================================
// root_note 決定
// WAV の生 PCM データを受け取り、モードに従って最終的な
// MIDI ノート番号を返す。
// ============================================================
struct RootNoteDecision {
    int  midiNote;          // 最終的な MIDI ノート番号
    bool wasEstimated;      // YIN 推定を実際に実行したか
    bool estimateSucceeded; // 推定が信頼度閾値を超えたか
    float estimatedFreq;    // 推定周波数 [Hz]（推定しなかった場合は 0）
    float confidence;       // YIN 信頼度（推定しなかった場合は 0）
};

static RootNoteDecision resolveRootNote(
    const WavEntry&              entry,
    const std::vector<uint8_t>&  wavRaw)
{
    constexpr int DEFAULT_NOTE = 69;

    RootNoteDecision result{};
    result.estimatedFreq = 0.0f;
    result.confidence    = 0.0f;

    // Fixed: 指定値をそのまま使う
    if (entry.rootNoteMode == RootNoteMode::Fixed) {
        result.midiNote          = entry.rootNoteFixed;
        result.wasEstimated      = false;
        result.estimateSucceeded = false;
        result.midiNote          = normalizeToOctave(result.midiNote, entry.octave);
        return result;
    }

    // None: デフォルト値
    if (entry.rootNoteMode == RootNoteMode::None) {
        result.midiNote          = normalizeToOctave(DEFAULT_NOTE, entry.octave);
        result.wasEstimated      = false;
        result.estimateSucceeded = false;
        return result;
    }

    // Auto: YIN で推定
    result.wasEstimated = true;

    std::vector<int16_t> mono;
    uint32_t             wavSampleRate = 0;
    try {
        auto [m, sr] = extractMonoPcm(wavRaw.data(), wavRaw.size());
        mono         = std::move(m);
        wavSampleRate = sr;
    } catch (const std::exception& e) {
        std::cerr << "[warn] PCM 取り出し失敗 (" << e.what()
                  << ") → root_note をデフォルト値 " << DEFAULT_NOTE << " にフォールバックします\n";
        result.midiNote          = normalizeToOctave(DEFAULT_NOTE, entry.octave);
        result.estimateSucceeded = false;
        return result;
    }

    PitchResult pitch = estimatePitch(mono, wavSampleRate);
    result.estimatedFreq = pitch.frequency;
    result.confidence    = pitch.confidence;

    if (pitch.frequency <= 0.0f || pitch.confidence < YIN_CONFIDENCE_THRESHOLD) {
        std::cerr << "[warn] ピッチ推定の信頼度が低い ("
                  << entry.path
                  << ", freq=" << pitch.frequency << " Hz"
                  << ", confidence=" << pitch.confidence << ")"
                  << " → root_note をデフォルト値 " << DEFAULT_NOTE << " にフォールバックします\n";
        result.midiNote          = normalizeToOctave(DEFAULT_NOTE, entry.octave);
        result.estimateSucceeded = false;
        return result;
    }

    int estimated = freqToMidiNote(pitch.frequency);
    result.midiNote          = normalizeToOctave(estimated, entry.octave);
    result.estimateSucceeded = true;
    return result;
}

// ============================================================
// リサンプリング後サンプルインデックスへの変換
//
// codec.cpp の resampling() は蓄積誤差法でソースサンプルごとに
// dstRate を加算し、srcRate を超えるたびに1サンプル出力する。
// これは N 個のソースサンプルを処理した時点の出力サンプル数が
// floor(N * dstRate / srcRate) に厳密に一致する（剰余がそのまま
// 次回へ持ち越されるため）。ループポイント変換はこの式をそのまま
// 使うことで、encode() が生成するADPCMニブル列のインデックスと
// 1:1で対応させる。
// ============================================================
static uint32_t mapSampleIndexToResampled(uint32_t srcIndex, uint32_t srcRate, uint32_t dstRate)
{
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(srcIndex) * dstRate) / srcRate);
}

// ============================================================
// loop (ループポイント) 決定
// WAV の smpl チャンクを優先し、無ければ YIN ベースの自動検出に
// フォールバックする。値はリサンプリング後 (エンコード対象) の
// サンプル単位で返す。
// ============================================================
struct LoopDecision {
    bool        hasLoop     = false;
    uint32_t    startSample = 0; // リサンプリング後サンプル単位 (包含)
    uint32_t    endSample   = 0; // リサンプリング後サンプル単位 (包含)
    std::string source;          // "smpl_chunk" / "auto_detected" / "fixed"
};

static LoopDecision resolveLoop(
    const WavEntry&              entry,
    const std::vector<uint8_t>&  wavRaw,
    uint32_t                     targetSampleRate)
{
    LoopDecision result{};

    if (entry.loopMode == LoopMode::None) {
        return result;
    }

    // Fixed: ユーザー指定値をそのまま使う (リサンプリング後サンプル単位で指定される想定。
    // auto 実行結果の loop_start_sample/loop_end_sample をそのまま書き戻せるようにする)
    if (entry.loopMode == LoopMode::Fixed) {
        result.hasLoop     = true;
        result.startSample = entry.loopFixedStart;
        result.endSample   = entry.loopFixedEnd;
        result.source      = "fixed";
        return result;
    }

    // Auto: smpl チャンク判定・自動検出のいずれにも元WAVのPCM/サンプルレートが必要
    std::vector<int16_t> mono;
    uint32_t              wavSampleRate = 0;
    try {
        auto [m, sr] = extractMonoPcm(wavRaw.data(), wavRaw.size());
        mono          = std::move(m);
        wavSampleRate = sr;
    } catch (const std::exception& e) {
        std::cerr << "[warn] PCM 取り出し失敗 (" << e.what()
                  << ") → ループポイント検出をスキップします\n";
        return result;
    }

    // 1. smpl チャンク優先
    if (auto smpl = readSmplLoop(wavRaw.data(), wavRaw.size())) {
        uint32_t start = mapSampleIndexToResampled(smpl->first,  wavSampleRate, targetSampleRate);
        uint32_t end   = mapSampleIndexToResampled(smpl->second, wavSampleRate, targetSampleRate);
        if (end > start) {
            result.hasLoop     = true;
            result.startSample = start;
            result.endSample   = end;
            result.source      = "smpl_chunk";
            return result;
        }
        std::cerr << "[warn] smpl チャンクのループ範囲がリサンプリング後に潰れました ("
                  << entry.path << ") → 自動検出にフォールバックします\n";
    }

    // 2. YIN ベースの自動検出
    LoopDetectResult detected = detectLoopPoints(mono, wavSampleRate);
    if (!detected.found) {
        std::cerr << "[warn] ループポイントの自動検出に失敗しました (" << entry.path
                  << ") → ループ情報なしで出力します\n";
        return result;
    }

    result.hasLoop     = true;
    result.startSample = mapSampleIndexToResampled(detected.startSample, wavSampleRate, targetSampleRate);
    result.endSample   = mapSampleIndexToResampled(detected.endSample,   wavSampleRate, targetSampleRate);
    result.source      = "auto_detected";

    std::cout << "\n    [loop] auto detected  confidence=" << detected.confidence
              << "  start=" << result.startSample << " end=" << result.endSample << " ";

    return result;
}

// ============================================================
// メイン処理
// ============================================================
int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: adpcm_packer <params.json>\n";
        return 1;
    }

    // --- パラメータ読み込み ---
    Params params;
    try {
        params = loadParams(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << "[error] パラメータ読み込み失敗: " << e.what() << "\n";
        return 1;
    }

    std::cout << "codec      : " << params.codec      << "\n";
    if (codecHasFormat(params.kind))
        std::cout << "format     : " << params.format << "\n";
    std::cout << "sample_rate: " << params.sampleRate << " Hz\n";
    std::cout << "boundary   : " << params.boundary   << " bytes\n";
    if (params.memorySize > 0)
        std::cout << "memory_size: " << params.memorySize << " bytes\n";
    std::cout << "output_bin : " << params.outputBin  << "\n";
    std::cout << "output_json: " << params.outputJson << "\n";
    std::cout << "wav files  : " << params.wavFiles.size() << " file(s)\n\n";

    // --- エンコーダ生成 ---
    std::unique_ptr<AdpcmEncoder> encoder;
    switch (params.kind) {
        case CodecKind::AdpcmB:      encoder = std::make_unique<YmDeltaTEncoder>();     break;
        case CodecKind::AdpcmA:      encoder = std::make_unique<Ym2610AEncoder>();      break;
        case CodecKind::Ymz280Adpcm: encoder = std::make_unique<Ymz280AdpcmEncoder>();  break;
        case CodecKind::Ymz280Pcm8:  encoder = std::make_unique<LinearPcm8Encoder>();   break;
        case CodecKind::Ymz280Pcm16: encoder = std::make_unique<LinearPcm16LEEncoder>();break;
        case CodecKind::Opl4Pcm8:    encoder = std::make_unique<LinearPcm8Encoder>();   break;
        case CodecKind::Opl4Pcm12:   encoder = std::make_unique<Opl4Pcm12Encoder>();    break;
        case CodecKind::Opl4Pcm16:   encoder = std::make_unique<LinearPcm16BEEncoder>();break;
        // SSGS の 4bit ADPCM は YMZ280B と同一フォーマット
        case CodecKind::Ssgs:        encoder = std::make_unique<Ymz280AdpcmEncoder>();  break;
    }

    // --- 各WAVをエンコードしてバイナリに結合 ---
    struct EntryInfo {
        std::string name;
        uint32_t    offset;
        uint32_t    size;
        uint32_t    paddedSize;
        int         rootNote;
        bool        hasLoop;
        uint32_t    loopStartSample; // リサンプリング後サンプル単位 (包含)
        uint32_t    loopEndSample;   // 同上 (包含)
        uint32_t    loopStartByte;   // 出力バイナリ内の絶対バイトオフセット
        uint32_t    loopEndByte;     // 同上 (包含)
        std::string loopSource;
        uint32_t    sampleRate;      // ymz280/opl4/ssgs のみ有効。そのエントリで実際に使ったレート
        uint32_t    endAddress;      // 実データ末尾の絶対アドレス (包含)。ssgs のROMテーブル用
    };

    const bool       isSsgs     = (params.kind == CodecKind::Ssgs);
    const SampleBank sampleBank = sampleBankFor(params.kind);

    std::vector<EntryInfo> entries;
    std::vector<uint8_t>   binData;
    uint32_t               currentOffset = 0;

    // SSGS はボイスアドレステーブルの分だけ先頭を空けておく (後段で書き込む)
    if (isSsgs) {
        binData.resize(ssgs::DATA_AREA_BASE, 0x00);
        currentOffset = ssgs::DATA_AREA_BASE;
    }

    for (auto& we : params.wavFiles) {
        std::cout << "  エンコード中: " << we.path << " ... ";
        std::cout.flush();

        std::vector<uint8_t> wavRaw;
        try {
            wavRaw = readFile(we.path);
        } catch (const std::exception& e) {
            std::cerr << "\n[error] " << e.what() << "\n";
            return 1;
        }

        // --- root_note 決定 ---
        RootNoteDecision rnd = resolveRootNote(we, wavRaw);

        // 推定結果のログ
        if (rnd.wasEstimated) {
            if (rnd.estimateSucceeded) {
                std::cout << "\n    [pitch] "
                          << rnd.estimatedFreq << " Hz"
                          << "  confidence=" << rnd.confidence
                          << "  → MIDI " << rnd.midiNote << " ";
            } else {
                // 警告は resolveRootNote 内で出力済み
                std::cout << "\n    [pitch] fallback → MIDI " << rnd.midiNote << " ";
            }
        }

        // --- このエントリで実際に使うサンプルレート (ymz280/opl4 は per-entry 上書き可) ---
        uint32_t entryRate = (we.hasSampleRateOverride) ? we.sampleRateOverride : params.sampleRate;

        // --- loop 決定 ---
        LoopDecision loopDec = resolveLoop(we, wavRaw, entryRate);

        // --- ADPCM エンコード ---
        DWORD adpcmSize = 0;
        BYTE* pAdpcm = encoder->waveToAdpcm(
            wavRaw.data(),
            static_cast<DWORD>(wavRaw.size()),
            adpcmSize,
            entryRate
        );

        if (pAdpcm == nullptr) {
            std::cerr << "\n[error] エンコード失敗: " << we.path
                      << "\n        (16bit リニア PCM WAV を使用してください)\n";
            return 1;
        }

        uint32_t entryOffset = alignUp(currentOffset, params.boundary);
        uint32_t paddedSize  = alignUp(adpcmSize, params.boundary);

        // boundary (32/256) はバンクサイズの約数なので、パディング込みの範囲で
        // 判定しても実データの範囲で判定しても結果は同じになる
        if (sampleBank.size > 0 && paddedSize > 0) {
            if (paddedSize > sampleBank.size) {
                std::cerr << "\n[error] " << params.codec << " の1サンプルは " << sampleBank.label
                          << " 以下にしてください: " << we.path
                          << " (padded " << paddedSize << " bytes)\n";
                delete[] pAdpcm;
                return 1;
            }
            const uint32_t lastByte = entryOffset + paddedSize - 1;
            if (entryOffset / sampleBank.size != lastByte / sampleBank.size) {
                const uint32_t moved = alignUp(entryOffset, sampleBank.size);
                std::cout << "\n    [align] " << sampleBank.label << " 境界をまたぐため 0x"
                          << std::hex << entryOffset << " -> 0x" << moved << std::dec << " へ移動 ";
                entryOffset = moved;
            }
        }

        uint32_t endAddress  = (adpcmSize > 0) ? (entryOffset + adpcmSize - 1) : entryOffset;

        if (isSsgs && entryOffset + paddedSize - 1 > ssgs::MAX_ADDRESS) {
            std::cerr << "\n[error] 出力サイズが SSGS の外部メモリ空間 (8Mbyte) を超えます: "
                      << we.path << "\n";
            delete[] pAdpcm;
            return 1;
        }

        if (params.memorySize > 0 &&
            static_cast<uint64_t>(entryOffset) + paddedSize > params.memorySize) {
            std::cerr << "\n[error] 出力サイズが memory_size (" << params.memorySize
                      << " bytes) を超えます: " << we.path << " (配置末尾 0x" << std::hex
                      << (entryOffset + paddedSize - 1) << std::dec << ")\n";
            delete[] pAdpcm;
            return 1;
        }

        // --- loop サンプル範囲 → バイトオフセット変換 ---
        // resampling() は 64サンプル境界まで無音パディングしてからエンコードするため、
        // 実データ末尾 (encoder->bytesToSamples(adpcmSize) サンプル) を超える範囲は
        // 縮めて安全側に倒す。サンプル数⇔バイト数の比率はフォーマットごとに異なるため
        // encoder 経由 (samplesToBytes/bytesToSamples) で変換する。
        bool     entryHasLoop = false;
        uint32_t startSample = 0, endSample = 0;
        uint32_t loopStartByte = 0, loopEndByte = 0;
        if (loopDec.hasLoop) {
            const uint32_t totalResampledSamples = encoder->bytesToSamples(adpcmSize);
            startSample = std::min(loopDec.startSample, totalResampledSamples > 0 ? totalResampledSamples - 1 : 0);
            endSample   = std::min(loopDec.endSample,   totalResampledSamples > 0 ? totalResampledSamples - 1 : 0);
            if (totalResampledSamples > 0 && endSample > startSample) {
                entryHasLoop = true;
                loopStartByte = entryOffset + encoder->samplesToBytes(startSample);
                loopEndByte   = entryOffset + encoder->samplesToBytes(endSample);
            } else {
                std::cerr << "[warn] loop 範囲がエンコード結果の範囲外のため無視します ("
                          << we.path << ")\n";
            }
        }

        entries.push_back({
            we.name, entryOffset, adpcmSize, paddedSize, rnd.midiNote,
            entryHasLoop, startSample, endSample, loopStartByte, loopEndByte, loopDec.source,
            entryRate, endAddress
        });

        binData.resize(entryOffset, 0x00); // boundary 整列によるギャップを埋める
        binData.insert(binData.end(), pAdpcm, pAdpcm + adpcmSize);
        binData.resize(entryOffset + paddedSize, 0x00);
        currentOffset = entryOffset + paddedSize;
        delete[] pAdpcm;

        std::cout << "OK  (" << adpcmSize << " bytes -> padded " << paddedSize
                  << " bytes, offset 0x" << std::hex << entries.back().offset
                  << std::dec << ")\n";
    }

    // --- SSGS ボイスアドレステーブルの書き込み ---
    if (isSsgs) {
        for (size_t i = 0; i < entries.size(); ++i) {
            writeSsgsVoiceTable(binData, static_cast<uint32_t>(i),
                                entries[i].offset, entries[i].endAddress);
        }
        std::cout << "\nボイステーブル: " << entries.size() << " 音 / 最大 "
                  << ssgs::MAX_VOICES << " 音\n";
    }

    // --- バイナリ出力 ---
    {
        std::ofstream f(params.outputBin, std::ios::binary);
        if (!f) {
            std::cerr << "[error] Cannot write: " << params.outputBin << "\n";
            return 1;
        }
        f.write(reinterpret_cast<const char*>(binData.data()),
                static_cast<std::streamsize>(binData.size()));
        std::cout << "\n出力バイナリ: " << params.outputBin
                  << "  (" << binData.size() << " bytes)\n";
    }

    // --- オフセット一覧 JSON 出力 (nlohmann/json 使用) ---
    {
        json out;
        out["codec"]       = params.codec;
        if (codecHasFormat(params.kind))
            out["format"] = params.format;
        out["sample_rate"] = params.sampleRate;
        out["boundary"]    = params.boundary;
        out["total_size"]  = static_cast<uint32_t>(binData.size());
        if (isSsgs) {
            char hexBase[16];
            std::snprintf(hexBase, sizeof(hexBase), "0x%06X", ssgs::DATA_AREA_BASE);
            out["data_area_offset"]     = ssgs::DATA_AREA_BASE;
            out["data_area_offset_hex"] = hexBase;
        }

        json arr = json::array();
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            char hexOff[16], hexEnd[16];
            std::snprintf(hexOff, sizeof(hexOff), "0x%06X", e.offset);
            std::snprintf(hexEnd, sizeof(hexEnd), "0x%06X", e.offset + e.paddedSize - 1);

            json entry;
            entry["name"]        = e.name;
            entry["offset"]      = e.offset;
            entry["offset_hex"]  = hexOff;
            entry["size"]        = e.size;
            entry["padded_size"] = e.paddedSize;
            entry["end_hex"]     = hexEnd;
            entry["root_note"]   = e.rootNote;
            if (codecSupportsPerEntryRate(params.kind))
                entry["sample_rate"] = e.sampleRate;
            if (isSsgs) {
                // ROM のボイスアドレステーブルに書き込んだ値と同じもの
                char hexEndAddr[16];
                std::snprintf(hexEndAddr, sizeof(hexEndAddr), "0x%06X", e.endAddress);
                entry["voice_no"]          = static_cast<uint32_t>(i);
                entry["start_address"]     = e.offset;
                entry["start_address_hex"] = hexOff;
                entry["end_address"]       = e.endAddress;
                entry["end_address_hex"]   = hexEndAddr;
                entry["sampling_code"]     = ssgs::samplingCode(e.sampleRate);
            }
            if (e.hasLoop) {
                char hexLoopStart[16], hexLoopEnd[16];
                std::snprintf(hexLoopStart, sizeof(hexLoopStart), "0x%06X", e.loopStartByte);
                std::snprintf(hexLoopEnd,   sizeof(hexLoopEnd),   "0x%06X", e.loopEndByte);
                entry["loop_start_sample"] = e.loopStartSample;
                entry["loop_end_sample"]   = e.loopEndSample;
                entry["loop_start_hex"]    = hexLoopStart;
                entry["loop_end_hex"]      = hexLoopEnd;
                entry["loop_source"]       = e.loopSource;
            }
            arr.push_back(std::move(entry));
        }
        out["entries"] = std::move(arr);

        std::ofstream f(params.outputJson);
        if (!f) {
            std::cerr << "[error] Cannot write: " << params.outputJson << "\n";
            return 1;
        }
        f << out.dump(2) << "\n";
        std::cout << "出力JSON   : " << params.outputJson << "\n";
    }

    std::cout << "\n完了。\n";
    return 0;
}
