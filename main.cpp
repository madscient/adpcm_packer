// adpcm_packer  -  WAV → ADPCM バイナリパッカー
//
// 使い方:
//   adpcm_packer <params.json>

#include "codec.h"
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
struct WavEntry {
    std::string path;
    std::string name;
};

struct Params {
    std::string           codec;
    uint32_t              sampleRate;
    uint32_t              boundary;
    std::string           outputBin;
    std::string           outputJson;
    std::vector<WavEntry> wavFiles;
};

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
    p.codec = root["codec"].get<std::string>();
    std::transform(p.codec.begin(), p.codec.end(), p.codec.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

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

    auto autoName = [](const std::string& path) -> std::string {
        auto sep = path.find_last_of("/\\");
        std::string name = (sep == std::string::npos) ? path : path.substr(sep + 1);
        if (name.size() > 4 && name.substr(name.size() - 4) == ".wav")
            name = name.substr(0, name.size() - 4);
        return name;
    };

    for (auto& item : root["wav_files"]) {
        WavEntry entry;
        if (item.is_string()) {
            entry.path = item.get<std::string>();
            entry.name = autoName(entry.path);
        } else if (item.is_object()) {
            if (!item.contains("path") || !item["path"].is_string())
                throw std::runtime_error("wav_files の各オブジェクト要素に 'path' (string) が必要です");
            entry.path = item["path"].get<std::string>();
            entry.name = item.contains("name") && item["name"].is_string()
                       ? item["name"].get<std::string>()
                       : autoName(entry.path);
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

        entries.push_back({ we.name, currentOffset, adpcmSize, paddedSize });

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
