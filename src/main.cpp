// adpcm_packer  -  WAV → ADPCM バイナリパッカー
//
// 使い方:
//   adpcm_packer <params.json>

#include "codec.h"
#include "wav_reader.h"
#include "pitch_estimator.h"
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

struct WavEntry {
    std::string  path;
    std::string  name;
    RootNoteMode rootNoteMode = RootNoteMode::None;
    int          rootNoteFixed = 69; // mode == Fixed のときのみ有効
    int          octave = -1;        // -1 = 制約なし ("none" または省略)
};

struct Params {
    std::string           codec;
    uint32_t              sampleRate = 0;
    uint32_t              boundary   = 0;
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
    if (p.codec != "adpcm-b" && p.codec != "adpcm-a")
        throw std::runtime_error("'codec' must be 'adpcm-b' or 'adpcm-a'");

    // --- sample_rate ---
    if (p.codec == "adpcm-a") {
        p.sampleRate = 18518;
        if (root.contains("sample_rate"))
            std::cerr << "[warn] ADPCM-A の sample_rate は 18518 Hz 固定です。指定値は無視されます。\n";
    } else {
        if (!root.contains("sample_rate") || !root["sample_rate"].is_number_integer())
            throw std::runtime_error("Missing or invalid 'sample_rate' field (integer required)");
        uint32_t sr = root["sample_rate"].get<uint32_t>();
        if (sr != 8000 && sr != 16000 && sr != 24000 && sr != 32000)
            throw std::runtime_error("ADPCM-B の sample_rate は 8000/16000/24000/32000 Hz のいずれかです");
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

    for (auto& item : root["wav_files"]) {
        WavEntry entry;
        if (item.is_string()) {
            // 文字列簡略記法: root_note=none 扱い、octave=制約なし
            entry.path          = item.get<std::string>();
            entry.name          = autoName(entry.path);
            entry.rootNoteMode  = RootNoteMode::None;
            entry.rootNoteFixed = 69;
            entry.octave        = -1;
        } else if (item.is_object()) {
            if (!item.contains("path") || !item["path"].is_string())
                throw std::runtime_error("wav_files の各オブジェクト要素に 'path' (string) が必要です");
            entry.path = item["path"].get<std::string>();
            entry.name = item.contains("name") && item["name"].is_string()
                       ? item["name"].get<std::string>()
                       : autoName(entry.path);
            parseRootNoteField(item, entry.path, entry.rootNoteMode, entry.rootNoteFixed);
            entry.octave = parseOctaveField(item, entry.path);
        } else {
            throw std::runtime_error("wav_files の各要素はパス文字列またはオブジェクトである必要があります");
        }
        p.wavFiles.push_back(std::move(entry));
    }

    if (p.wavFiles.empty())
        throw std::runtime_error("'wav_files' が空です");

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
    std::cout << "sample_rate: " << params.sampleRate << " Hz\n";
    std::cout << "boundary   : " << params.boundary   << " bytes\n";
    std::cout << "output_bin : " << params.outputBin  << "\n";
    std::cout << "output_json: " << params.outputJson << "\n";
    std::cout << "wav files  : " << params.wavFiles.size() << " file(s)\n\n";

    // --- エンコーダ生成 ---
    std::unique_ptr<AdpcmEncoder> encoder;
    if (params.codec == "adpcm-b")
        encoder = std::make_unique<YmDeltaTEncoder>();
    else
        encoder = std::make_unique<Ym2610AEncoder>();

    // --- 各WAVをエンコードしてバイナリに結合 ---
    struct EntryInfo {
        std::string name;
        uint32_t    offset;
        uint32_t    size;
        uint32_t    paddedSize;
        int         rootNote;
    };

    std::vector<EntryInfo> entries;
    std::vector<uint8_t>   binData;
    uint32_t               currentOffset = 0;

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

        // --- ADPCM エンコード ---
        DWORD adpcmSize = 0;
        BYTE* pAdpcm = encoder->waveToAdpcm(
            wavRaw.data(),
            static_cast<DWORD>(wavRaw.size()),
            adpcmSize,
            params.sampleRate
        );

        if (pAdpcm == nullptr) {
            std::cerr << "\n[error] エンコード失敗: " << we.path
                      << "\n        (16bit リニア PCM WAV を使用してください)\n";
            return 1;
        }

        uint32_t paddedSize = alignUp(adpcmSize, params.boundary);
        entries.push_back({ we.name, currentOffset, adpcmSize, paddedSize, rnd.midiNote });

        binData.insert(binData.end(), pAdpcm, pAdpcm + adpcmSize);
        binData.resize(currentOffset + paddedSize, 0x00);
        currentOffset += paddedSize;
        delete[] pAdpcm;

        std::cout << "OK  (" << adpcmSize << " bytes -> padded " << paddedSize
                  << " bytes, offset 0x" << std::hex << entries.back().offset
                  << std::dec << ")\n";
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
        out["sample_rate"] = params.sampleRate;
        out["boundary"]    = params.boundary;
        out["total_size"]  = static_cast<uint32_t>(binData.size());

        json arr = json::array();
        for (auto& e : entries) {
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
