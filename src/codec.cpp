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
    dAdpcmSize = samplesToBytes(dPcmSize);
    BYTE* pAdpcm = new BYTE[dAdpcmSize]();
    encode(pPcm, pAdpcm, dPcmSize);
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

// ============================================================
// Ymz280AdpcmEncoder  (YMZ280B 4bit ADPCM)
// superctr/adpcm の ymz_encode / ymz_step を移植したもの。
// ============================================================

const int Ymz280AdpcmEncoder::step_table[8] = {
    230, 230, 230, 230, 307, 409, 512, 614
};

int Ymz280AdpcmEncoder::encode(short* pSrc, unsigned char* pDis, DWORD iSampleSize)
{
    long stepSize = 127;
    long history   = 0;
    unsigned char adpcmPack = 0;

    for (DWORD iCnt = 0; iCnt < iSampleSize; ++iCnt) {
        // 精度を落としてノイズを低減する (superctr/adpcm 実装のコメントより)
        long diffIn = (static_cast<long>(*pSrc++) & ~7L) - history;
        long adpcmU = (std::abs(diffIn) << 16) / (stepSize << 14);
        if (adpcmU > 7) adpcmU = 7;
        unsigned char adpcm = static_cast<unsigned char>(adpcmU);
        if (diffIn < 0) adpcm |= 0x8;

        // --- 実チップのデコーダと同じ更新式で history / stepSize を進める ---
        long delta = adpcm & 0x7;
        long diff  = ((1 + (delta << 1)) * stepSize) >> 3;
        if (diff < 0)     diff = 0;
        if (diff > 32767) diff = 32767;
        long newHistory = ((adpcm & 0x8) != 0) ? (history - diff) : (history + diff);
        if (newHistory < -32768) newHistory = -32768;
        if (newHistory > 32767)  newHistory = 32767;
        history = newHistory;

        long newStep = (step_table[delta] * stepSize) >> 8;
        if (newStep < 127)   newStep = 127;
        if (newStep > 24576) newStep = 24576;
        stepSize = newStep;

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
// LinearPcm8Encoder  (YMZ280B / OPL4 共通・符号付き8bit)
// ============================================================

int LinearPcm8Encoder::encode(short* pSrc, unsigned char* pDis, DWORD iSampleSize)
{
    for (DWORD i = 0; i < iSampleSize; ++i) {
        long v = static_cast<long>(pSrc[i]) + 128; // 丸め
        if (v > 32767) v = 32767;
        pDis[i] = static_cast<unsigned char>(v >> 8);
    }
    return 0;
}

// ============================================================
// LinearPcm16LEEncoder  (YMZ280B・16bit リトルエンディアン)
// ============================================================

int LinearPcm16LEEncoder::encode(short* pSrc, unsigned char* pDis, DWORD iSampleSize)
{
    for (DWORD i = 0; i < iSampleSize; ++i) {
        unsigned short v = static_cast<unsigned short>(pSrc[i]);
        pDis[i * 2 + 0] = static_cast<unsigned char>(v & 0xFF);
        pDis[i * 2 + 1] = static_cast<unsigned char>((v >> 8) & 0xFF);
    }
    return 0;
}

// ============================================================
// LinearPcm16BEEncoder  (OPL4/YMF278B・16bit ビッグエンディアン)
// ============================================================

int LinearPcm16BEEncoder::encode(short* pSrc, unsigned char* pDis, DWORD iSampleSize)
{
    for (DWORD i = 0; i < iSampleSize; ++i) {
        unsigned short v = static_cast<unsigned short>(pSrc[i]);
        pDis[i * 2 + 0] = static_cast<unsigned char>((v >> 8) & 0xFF);
        pDis[i * 2 + 1] = static_cast<unsigned char>(v & 0xFF);
    }
    return 0;
}

// ============================================================
// Opl4Pcm12Encoder  (OPL4/YMF278B・12bit パック PCM)
// 2サンプル(v0,v1)を3バイトに詰める:
//   byte0 = v0 の上位8bit
//   byte1 = (v1 の下位4bit << 4) | (v0 の下位4bit)
//   byte2 = v1 の上位8bit
// (ymfm の fetch_sample 12bit分岐のデコード式から逆算した詰め方)
// ============================================================

int Opl4Pcm12Encoder::encode(short* pSrc, unsigned char* pDis, DWORD iSampleSize)
{
    for (DWORD i = 0; i + 1 < iSampleSize; i += 2) {
        long v0 = static_cast<long>(pSrc[i])     >> 4; // 12bit符号付き (-2048~2047)
        long v1 = static_cast<long>(pSrc[i + 1]) >> 4;

        DWORD outBase = (i / 2) * 3;
        pDis[outBase + 0] = static_cast<unsigned char>((v0 >> 4) & 0xFF);
        pDis[outBase + 1] = static_cast<unsigned char>(((v1 & 0xF) << 4) | (v0 & 0xF));
        pDis[outBase + 2] = static_cast<unsigned char>((v1 >> 4) & 0xFF);
    }
    return 0;
}
