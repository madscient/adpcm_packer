#include "codec.h"
#include <cstdlib>   // abs
#include <cstring>   // memcpy, memset
#include <algorithm> // std::min / std::max

// ============================================================
// AdpcmEncoder 共通部
// ============================================================

BYTE* AdpcmEncoder::waveToAdpcm(void* pData, DWORD /*dSize*/, DWORD& dAdpcmSize, DWORD rate)
{
    // --- RIFF ヘッダ確認 ---
    m_pRiffHed = reinterpret_cast<RIFF_HED*>(pData);
    if (m_pRiffHed->bID[0] != 'R' || m_pRiffHed->bID[1] != 'I' ||
        m_pRiffHed->bID[2] != 'F' || m_pRiffHed->bID[3] != 'F') {
        return nullptr;
    }

    // --- WAVE チャンク確認 ---
    m_pWaveChunk = reinterpret_cast<WAVE_CHUNK*>(static_cast<BYTE*>(pData) + 8);
    if (m_pWaveChunk->bID[0] != 'W' || m_pWaveChunk->bID[1] != 'A' ||
        m_pWaveChunk->bID[2] != 'V' || m_pWaveChunk->bID[3] != 'E') {
        return nullptr;
    }
    if (m_pWaveChunk->bFMT[0] != 'f' || m_pWaveChunk->bFMT[1] != 'm' ||
        m_pWaveChunk->bFMT[2] != 't' || m_pWaveChunk->bFMT[3] != ' ') {
        return nullptr;
    }
    if (m_pWaveChunk->wFmt != 0x0001) { // リニア PCM のみ
        return nullptr;
    }

    // --- DATA チャンクへのポインタ ---
    m_pDataChunk = reinterpret_cast<DATA_CHUNK*>(
        reinterpret_cast<BYTE*>(&m_pWaveChunk->wFmt) + m_pWaveChunk->dChunkSize);

    // "data" チャンクを探す（fmt チャンクの直後に来るとは限らないWAVへの対応）
    // 簡易版: bID が "data" でなければポインタを進める
    auto* base = static_cast<BYTE*>(pData);
    auto* end  = base + m_pRiffHed->dSize + 8;
    while (reinterpret_cast<BYTE*>(m_pDataChunk) + 8 < end) {
        if (m_pDataChunk->bID[0] == 'd' && m_pDataChunk->bID[1] == 'a' &&
            m_pDataChunk->bID[2] == 't' && m_pDataChunk->bID[3] == 'a') {
            break;
        }
        // 次のチャンクへ
        DWORD skip = m_pDataChunk->dSize; // bIDとdSizeで8バイト
        // bID(4) + dSize(4) = 8 バイトオフセット
        m_pDataChunk = reinterpret_cast<DATA_CHUNK*>(
            reinterpret_cast<BYTE*>(m_pDataChunk) + 8 + skip);
    }

    // --- リサンプリング ---
    DWORD  dPcmSize = 0;
    short* pPcm     = resampling(dPcmSize, rate);
    if (pPcm == nullptr) {
        return nullptr;
    }

    // --- ADPCM エンコード ---
    BYTE* pAdpcm = new BYTE[dPcmSize / 2]();
    encode(pPcm, pAdpcm, dPcmSize);
    dAdpcmSize = dPcmSize / 2;
    delete[] pPcm;
    return pAdpcm;
}

short* AdpcmEncoder::resampling(DWORD& dSize, DWORD rate)
{
    if (m_pWaveChunk->wSample != 16) {
        return nullptr; // 16bit PCM のみ対応
    }

    // --- モノラル化 ---
    short* pPcm    = nullptr;
    int    iPcmSize = 0;

    if (m_pWaveChunk->wChannels == 2) {
        iPcmSize = static_cast<int>(m_pDataChunk->dSize / 4);
        pPcm = new short[iPcmSize];
        const short* pSrc = reinterpret_cast<const short*>(&m_pDataChunk->bData[0]);
        for (int i = 0; i < iPcmSize; ++i) {
            int v = static_cast<int>(pSrc[0]) + static_cast<int>(pSrc[1]);
            pPcm[i] = static_cast<short>(v / 2);
            pSrc += 2;
        }
    } else if (m_pWaveChunk->wChannels == 1) {
        iPcmSize = static_cast<int>(m_pDataChunk->dSize / 2);
        pPcm = new short[iPcmSize];
        std::memcpy(pPcm, &m_pDataChunk->bData[0], m_pDataChunk->dSize);
    } else {
        return nullptr;
    }

    // --- リサンプリング後サンプル数を計算 ---
    const int iSrcRate = static_cast<int>(m_pWaveChunk->dRate);
    const int iDisRate = static_cast<int>(rate);
    int iDiff       = 0;
    int iSampleSize = 0;

    for (int i = 0; i < iPcmSize; ++i) {
        iDiff += iDisRate;
        while (iDiff >= iSrcRate) {
            ++iSampleSize;
            iDiff -= iSrcRate;
        }
    }
    if (iDiff > 0) ++iSampleSize;

    // 64サンプル境界に切り上げ（エンコーダの要件）
    int iResampleBuffSize = iSampleSize;
    if (iSampleSize % 64 > 0) {
        iResampleBuffSize += (64 - (iSampleSize % 64));
    }

    short* pResampleBuff = new short[iResampleBuffSize]();

    // --- リサンプリング本体（平均値ダウンサンプリング） ---
    short* pDst       = pResampleBuff;
    int    iSmple     = 0;
    int    iSampleCnt = 0;
    bool   bUpdate    = false;
    iDiff = 0;

    for (int i = 0; i < iPcmSize; ++i) {
        iSmple     += static_cast<int>(pPcm[i]);
        ++iSampleCnt;
        iDiff += iDisRate;
        bUpdate = false;
        while (iDiff >= iSrcRate) {
            *pDst++ = static_cast<short>(iSmple / iSampleCnt);
            iDiff -= iSrcRate;
            bUpdate = true;
        }
        if (bUpdate) {
            iSmple     = 0;
            iSampleCnt = 0;
        }
    }
    if (iSampleCnt > 0) {
        *pDst++ = static_cast<short>(iSmple / iSampleCnt);
    }

    delete[] pPcm;
    dSize = static_cast<DWORD>(iResampleBuffSize);
    return pResampleBuff;
}

// ============================================================
// YmDeltaTEncoder  (ADPCM-B)
// ============================================================

const long YmDeltaTEncoder::stepsizeTable[16] = {
    57, 57, 57, 57, 77, 102, 128, 153,
    57, 57, 57, 57, 77, 102, 128, 153
};

int YmDeltaTEncoder::encode(short* pSrc, unsigned char* pDis, DWORD iSampleSize)
{
    long xn       = 0;
    long stepSize = 127;
    unsigned char adpcmPack = 0;

    for (DWORD iCnt = 0; iCnt < iSampleSize; ++iCnt) {
        long dn = static_cast<long>(*pSrc++) - xn;
        long i  = (std::abs(dn) << 16) / (stepSize << 14);
        if (i > 7) i = 7;

        unsigned char adpcm = static_cast<unsigned char>(i);
        long delta = (static_cast<long>(adpcm) * 2 + 1) * stepSize >> 3;

        if (dn < 0) {
            adpcm |= 0x8;
            xn -= delta;
        } else {
            xn += delta;
        }

        stepSize = (stepsizeTable[adpcm] * stepSize) / 64;
        if (stepSize < 127)   stepSize = 127;
        if (stepSize > 24576) stepSize = 24576;

        if ((iCnt & 0x01) == 0) {
            adpcmPack = static_cast<unsigned char>(adpcm << 4);
        } else {
            adpcmPack |= adpcm;
            *pDis++ = adpcmPack;
        }
    }
    return 0;
}

// ============================================================
// Ym2610AEncoder  (ADPCM-A)
// ============================================================

const short Ym2610AEncoder::step_size[49] = {
    16,  17,  19,  21,  23,  25,  28,  31,  34,  37,
    41,  45,  50,  55,  60,  66,  73,  80,  88,  97,
   107, 118, 130, 143, 157, 173, 190, 209, 230, 253,
   279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
   724, 796, 876, 963,1060,1166,1282,1411,1552
};

const int Ym2610AEncoder::step_adj[16] = {
    -1, -1, -1, -1, 2, 5, 7, 9,
    -1, -1, -1, -1, 2, 5, 7, 9
};

Ym2610AEncoder::Ym2610AEncoder()
{
    jedi_table_init();
}

Ym2610AEncoder::~Ym2610AEncoder()
{
    delete[] jedi_table;
}

void Ym2610AEncoder::jedi_table_init()
{
    jedi_table = new int[16 * 49];
    for (int s = 0; s < 49; ++s) {
        for (int n = 0; n < 16; ++n) {
            int value = (2 * (n & 0x07) + 1) * step_size[s] / 8;
            jedi_table[s * 16 + n] = ((n & 0x08) != 0) ? -value : value;
        }
    }
}

short Ym2610AEncoder::YM2610_ADPCM_A_Decode(byte code)
{
    acc += jedi_table[decstep + code];
    if ((acc & ~0x7ff) != 0)
        acc |= ~0xfff;
    else
        acc &= 0xfff;
    decstep += step_adj[code & 7] * 16;
    if (decstep < 0)       decstep = 0;
    if (decstep > 48 * 16) decstep = 48 * 16;
    return static_cast<short>(acc);
}

byte Ym2610AEncoder::YM2610_ADPCM_A_Encode(short sample)
{
    predsample = prevsample;
    index      = previndex;
    step       = step_size[index];
    diff       = sample - predsample;

    byte code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }

    int tempstep = step;
    if (diff >= tempstep) { code |= 4; diff -= tempstep; }
    tempstep >>= 1;
    if (diff >= tempstep) { code |= 2; diff -= tempstep; }
    tempstep >>= 1;
    if (diff >= tempstep)   code |= 1;

    predsample = YM2610_ADPCM_A_Decode(code);

    index += step_adj[code];
    if (index < 0)  index = 0;
    if (index > 48) index = 48;

    prevsample = predsample;
    previndex  = index;
    return code;
}

int Ym2610AEncoder::encode(short* pSrc, unsigned char* pDis, DWORD iSampleSize)
{
    // reset state
    acc = 0; decstep = 0; prevsample = 0; previndex = 0;

    if (iSampleSize & 1) ++iSampleSize;
    inBuffer = new short[iSampleSize]();

    // 12bit へダウンスケール
    for (DWORD i = 0; i < iSampleSize; ++i) {
        inBuffer[i] = pSrc[i] >> 4;
    }

    for (DWORD i = 0; i < iSampleSize; i += 2) {
        pDis[i / 2] = static_cast<byte>(
            (YM2610_ADPCM_A_Encode(inBuffer[i])     << 4) |
             YM2610_ADPCM_A_Encode(inBuffer[i + 1])
        );
    }

    delete[] inBuffer;
    inBuffer = nullptr;
    return 0;
}
