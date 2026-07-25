#pragma once
#include "codec.h"   // BYTE, WORD, DWORD, RIFF_HED, WAVE_CHUNK, DATA_CHUNK
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <cstring>
#include <optional>
#include <utility>

// WAV メモリイメージから 16bit モノラル PCM を取り出す。
// - ステレオの場合は L+R 平均でモノラル化する
// - 戻り値の second はサンプリング周波数 (Hz)
// - 16bit リニア PCM 以外は std::runtime_error を投げる
inline std::pair<std::vector<int16_t>, uint32_t>
    extractMonoPcm(const void* pData, size_t dataSize)
{
    if (dataSize < sizeof(RIFF_HED) + sizeof(WAVE_CHUNK))
        throw std::runtime_error("WAV データが短すぎます");

    const auto* riff = reinterpret_cast<const RIFF_HED*>(pData);
    if (riff->bID[0]!='R'||riff->bID[1]!='I'||riff->bID[2]!='F'||riff->bID[3]!='F')
        throw std::runtime_error("RIFF ヘッダが見つかりません");

    const auto* wave = reinterpret_cast<const WAVE_CHUNK*>(
        static_cast<const uint8_t*>(pData) + 8);
    if (wave->bID[0]!='W'||wave->bID[1]!='A'||wave->bID[2]!='V'||wave->bID[3]!='E')
        throw std::runtime_error("WAVE チャンクが見つかりません");
    if (wave->bFMT[0]!='f'||wave->bFMT[1]!='m'||wave->bFMT[2]!='t'||wave->bFMT[3]!=' ')
        throw std::runtime_error("fmt チャンクが見つかりません");
    if (wave->wFmt != 0x0001)
        throw std::runtime_error("リニア PCM 以外のフォーマットには対応していません");
    if (wave->wSample != 16)
        throw std::runtime_error("16bit PCM 以外には対応していません");

    // data チャンクを探す
    const auto* base = static_cast<const uint8_t*>(pData);
    const auto* end  = base + riff->dSize + 8;
    const auto* dc   = reinterpret_cast<const DATA_CHUNK*>(
        reinterpret_cast<const uint8_t*>(&wave->wFmt) + wave->dChunkSize);
    while (reinterpret_cast<const uint8_t*>(dc) + 8 < end) {
        if (dc->bID[0]=='d'&&dc->bID[1]=='a'&&dc->bID[2]=='t'&&dc->bID[3]=='a') break;
        dc = reinterpret_cast<const DATA_CHUNK*>(
            reinterpret_cast<const uint8_t*>(dc) + 8 + dc->dSize);
    }

    const auto*   src      = reinterpret_cast<const int16_t*>(&dc->bData[0]);
    const uint32_t ch      = wave->wChannels;
    const uint32_t nFrames = dc->dSize / (2 * ch);

    std::vector<int16_t> mono(nFrames);
    if (ch == 1) {
        std::memcpy(mono.data(), src, nFrames * sizeof(int16_t));
    } else if (ch == 2) {
        for (uint32_t i = 0; i < nFrames; ++i)
            mono[i] = static_cast<int16_t>((static_cast<int32_t>(src[i*2]) + src[i*2+1]) / 2);
    } else {
        throw std::runtime_error("3ch 以上の WAV には対応していません");
    }

    return { std::move(mono), wave->dRate };
}

// WAV の smpl チャンクから最初のループポイントを読み取る。
// 戻り値はサンプルフレーム単位 (start/end とも包含) の (start, end) ペア。
// smpl チャンクが無い・ループ定義が無い・不正な場合は std::nullopt を返す。
//
// 参照仕様: https://www.recordingblogs.com/wiki/sample-chunk-of-a-wave-file
// smpl チャンクは data チャンクの後に置かれることが多いため、data チャンク探索
// (extractMonoPcm) とは別に WAVE 直後から全チャンクを走査する。
inline std::optional<std::pair<uint32_t, uint32_t>>
    readSmplLoop(const void* pData, size_t dataSize)
{
    if (dataSize < sizeof(RIFF_HED) + 4) return std::nullopt;

    const auto* riff = reinterpret_cast<const RIFF_HED*>(pData);
    if (riff->bID[0]!='R'||riff->bID[1]!='I'||riff->bID[2]!='F'||riff->bID[3]!='F')
        return std::nullopt;

    const auto* base = static_cast<const uint8_t*>(pData);
    const auto* end   = base + std::min<size_t>(static_cast<size_t>(riff->dSize) + 8, dataSize);

    // "WAVE" (オフセット8〜11) の直後、オフセット12からチャンクを順に走査する
    const auto* chunk = reinterpret_cast<const DATA_CHUNK*>(base + 12);
    while (reinterpret_cast<const uint8_t*>(chunk) + 8 <= end) {
        if (chunk->bID[0]=='s'&&chunk->bID[1]=='m'&&chunk->bID[2]=='p'&&chunk->bID[3]=='l') {
            const uint8_t* p = chunk->bData; // manufacturer 以降のフィールド列
            // manufacturer,product,samplePeriod,midiUnityNote,midiPitchFraction,
            // smpteFormat,smpteOffset,numSampleLoops,samplerData = uint32 x 9 = 36 bytes
            if (p + 36 > end) return std::nullopt;
            uint32_t numLoops = 0;
            std::memcpy(&numLoops, p + 28, sizeof(uint32_t));
            if (numLoops < 1) return std::nullopt;

            // ループ定義1個目: cuePointId,type,start,end,fraction,playCount = uint32 x 6
            const uint8_t* loop0 = p + 36;
            if (loop0 + 24 > end) return std::nullopt;
            uint32_t loopStart = 0, loopEnd = 0;
            std::memcpy(&loopStart, loop0 + 8,  sizeof(uint32_t));
            std::memcpy(&loopEnd,   loop0 + 12, sizeof(uint32_t));
            if (loopEnd <= loopStart) return std::nullopt;
            return std::make_pair(loopStart, loopEnd);
        }
        uint32_t advance = chunk->dSize + (chunk->dSize & 1); // 奇数サイズは1バイトパディング
        chunk = reinterpret_cast<const DATA_CHUNK*>(
            reinterpret_cast<const uint8_t*>(chunk) + 8 + advance);
    }
    return std::nullopt;
}
