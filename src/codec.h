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

    // フォーマットごとのサンプル数⇔バイト数変換 (bytes-per-sample 比率)。
    // main.cpp 側でのバウンダリ整列・ループ点のバイトオフセット換算にも使う。
    virtual DWORD samplesToBytes(DWORD sampleCount) const = 0;
    virtual DWORD bytesToSamples(DWORD byteCount)   const = 0;

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

    DWORD samplesToBytes(DWORD sampleCount) const override { return sampleCount / 2; }
    DWORD bytesToSamples(DWORD byteCount)   const override { return byteCount * 2; }

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

    DWORD samplesToBytes(DWORD sampleCount) const override { return sampleCount / 2; }
    DWORD bytesToSamples(DWORD byteCount)   const override { return byteCount * 2; }

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

// ------------------------------------------------------------
// ADPCM (YMZ280B)
// superctr/adpcm の ymz_encode を移植。ヒストリ・ステップサイズ更新式は
// AICA と共通だが、ニブル順 (上位ニブルが先) が異なる。
// ------------------------------------------------------------
class Ymz280AdpcmEncoder : public AdpcmEncoder {
public:
    Ymz280AdpcmEncoder()  = default;
    ~Ymz280AdpcmEncoder() = default;

    DWORD samplesToBytes(DWORD sampleCount) const override { return sampleCount / 2; }
    DWORD bytesToSamples(DWORD byteCount)   const override { return byteCount * 2; }

protected:
    int encode(short* pSrc, unsigned char* pDis, DWORD iSampleSize) override;

private:
    static const int step_table[8];
};

// ------------------------------------------------------------
// 8bit リニア PCM (YMZ280B / OPL4 共通)
// 符号付き8bit。16bit サンプルの上位バイトを丸めて格納する。
// ------------------------------------------------------------
class LinearPcm8Encoder : public AdpcmEncoder {
public:
    LinearPcm8Encoder()  = default;
    ~LinearPcm8Encoder() = default;

    DWORD samplesToBytes(DWORD sampleCount) const override { return sampleCount; }
    DWORD bytesToSamples(DWORD byteCount)   const override { return byteCount; }

protected:
    int encode(short* pSrc, unsigned char* pDis, DWORD iSampleSize) override;
};

// ------------------------------------------------------------
// 16bit リニア PCM・リトルエンディアン (YMZ280B)
// ------------------------------------------------------------
class LinearPcm16LEEncoder : public AdpcmEncoder {
public:
    LinearPcm16LEEncoder()  = default;
    ~LinearPcm16LEEncoder() = default;

    DWORD samplesToBytes(DWORD sampleCount) const override { return sampleCount * 2; }
    DWORD bytesToSamples(DWORD byteCount)   const override { return byteCount / 2; }

protected:
    int encode(short* pSrc, unsigned char* pDis, DWORD iSampleSize) override;
};

// ------------------------------------------------------------
// 16bit リニア PCM・ビッグエンディアン (OPL4 / YMF278B)
// ------------------------------------------------------------
class LinearPcm16BEEncoder : public AdpcmEncoder {
public:
    LinearPcm16BEEncoder()  = default;
    ~LinearPcm16BEEncoder() = default;

    DWORD samplesToBytes(DWORD sampleCount) const override { return sampleCount * 2; }
    DWORD bytesToSamples(DWORD byteCount)   const override { return byteCount / 2; }

protected:
    int encode(short* pSrc, unsigned char* pDis, DWORD iSampleSize) override;
};

// ------------------------------------------------------------
// 12bit リニア PCM (OPL4 / YMF278B)
// 2サンプルを3バイトに詰める (ニブル交差パッキング)。
// ------------------------------------------------------------
class Opl4Pcm12Encoder : public AdpcmEncoder {
public:
    Opl4Pcm12Encoder()  = default;
    ~Opl4Pcm12Encoder() = default;

    DWORD samplesToBytes(DWORD sampleCount) const override { return (sampleCount / 2) * 3; }
    DWORD bytesToSamples(DWORD byteCount)   const override { return (byteCount / 3) * 2; }

protected:
    int encode(short* pSrc, unsigned char* pDis, DWORD iSampleSize) override;
};
