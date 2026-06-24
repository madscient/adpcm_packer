#pragma once
#include <cstdint>
#include <cstring>

// Windows型の代替定義
using BYTE   = uint8_t;
using WORD   = uint16_t;
using DWORD  = uint32_t;
using byte   = uint8_t;

#pragma pack(push, 1)
struct RIFF_HED {
    BYTE  bID[4];
    DWORD dSize;
};

struct WAVE_CHUNK {
    BYTE  bID[4];
    BYTE  bFMT[4];
    DWORD dChunkSize;
    WORD  wFmt;
    WORD  wChannels;
    DWORD dRate;
    DWORD dDataRate;
    WORD  wBlockSize;
    WORD  wSample;
};

struct DATA_CHUNK {
    BYTE  bID[4];
    DWORD dSize;
    BYTE  bData[1];
};
#pragma pack(pop)

// ------------------------------------------------------------
// 基底クラス
// ------------------------------------------------------------
class AdpcmEncoder {
public:
    AdpcmEncoder() = default;
    virtual ~AdpcmEncoder() = default;

    // WAVデータ(メモリ上) → ADPCMデータ(new[]で確保して返す)
    // 呼び出し側で delete[] すること
    BYTE* waveToAdpcm(void* pData, DWORD dSize, DWORD& dAdpcmSize, DWORD rate);

protected:
    RIFF_HED*   m_pRiffHed   = nullptr;
    WAVE_CHUNK* m_pWaveChunk = nullptr;
    DATA_CHUNK* m_pDataChunk = nullptr;

    // リサンプリング + モノラル化。呼び出し側で delete[] すること
    short* resampling(DWORD& dSize, DWORD rate);

    // サブクラスが実装するエンコード本体
    virtual int encode(short* pSrc, unsigned char* pDis, DWORD iSampleSize) = 0;
};

// ------------------------------------------------------------
// ADPCM-B (YM Delta-T)
// ------------------------------------------------------------
class YmDeltaTEncoder : public AdpcmEncoder {
public:
    YmDeltaTEncoder()  = default;
    ~YmDeltaTEncoder() = default;

protected:
    int encode(short* pSrc, unsigned char* pDis, DWORD iSampleSize) override;

private:
    static const long stepsizeTable[16];
};

// ------------------------------------------------------------
// ADPCM-A (YM2610)
// ------------------------------------------------------------
class Ym2610AEncoder : public AdpcmEncoder {
public:
    Ym2610AEncoder();
    ~Ym2610AEncoder();

protected:
    int encode(short* pSrc, unsigned char* pDis, DWORD iSampleSize) override;

private:
    static const short step_size[49];
    static const int   step_adj[16];

    short* inBuffer  = nullptr;
    int*   jedi_table = nullptr;

    // decode state
    int acc     = 0;
    int decstep = 0;

    // encode state
    int diff        = 0;
    int step        = 0;
    int predsample  = 0;
    int index       = 0;
    int prevsample  = 0;
    int previndex   = 0;

    void  jedi_table_init();
    byte  YM2610_ADPCM_A_Encode(short sample);
    short YM2610_ADPCM_A_Decode(byte code);
};
