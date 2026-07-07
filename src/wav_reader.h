#pragma once
#include "codec.h"   // BYTE, WORD, DWORD, RIFF_HED, WAVE_CHUNK, DATA_CHUNK
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <cstring>

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
