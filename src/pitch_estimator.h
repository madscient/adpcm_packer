#pragma once
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <optional>

// ============================================================
// YIN アルゴリズムによる基本周波数（F0）推定
//
// 参考: de Cheveigné & Kawahara (2002) "YIN, a fundamental
//       frequency estimator for speech and music"
//       J. Acoust. Soc. Am. 111(4), pp.1917-1930
//
// 推定対象: 16bit モノラル PCM（楽音の単音サンプル向け）
// 推定範囲: 50 Hz〜2000 Hz（MIDI note 31〜95 相当）
// ============================================================

struct PitchResult {
    float frequency  = 0.0f; // 推定基本周波数 [Hz]。0 は推定失敗
    float confidence = 0.0f; // 信頼度 [0.0〜1.0]。閾値: YIN_THRESHOLD
};

// 信頼度がこの値を下回る場合はフォールバックする
static constexpr float YIN_CONFIDENCE_THRESHOLD = 0.75f;

// ============================================================
// YIN 内部実装
// ============================================================
namespace yin_detail {

// ステップ1・2: 差分関数 d(tau) を計算
static std::vector<double> differenceFunction(
    const int16_t* samples, int frameSize, int maxTau)
{
    std::vector<double> df(maxTau + 1, 0.0);
    for (int tau = 1; tau <= maxTau; ++tau) {
        double sum = 0.0;
        for (int j = 0; j < frameSize; ++j) {
            double delta = static_cast<double>(samples[j]) -
                           static_cast<double>(samples[j + tau]);
            sum += delta * delta;
        }
        df[tau] = sum;
    }
    return df;
}

// ステップ3: 累積平均正規化差分関数 (CMNDF)
static std::vector<double> cmndf(const std::vector<double>& df)
{
    int n = static_cast<int>(df.size());
    std::vector<double> result(n, 0.0);
    result[0] = 1.0;
    double cumsum = 0.0;
    for (int tau = 1; tau < n; ++tau) {
        cumsum += df[tau];
        result[tau] = (cumsum > 0.0) ? (df[tau] * tau / cumsum) : 0.0;
    }
    return result;
}

// ステップ4: 閾値以下の最初の局所最小値を探す
static int absoluteThreshold(const std::vector<double>& cmdf, double threshold)
{
    int n = static_cast<int>(cmdf.size());
    for (int tau = 2; tau < n - 1; ++tau) {
        if (cmdf[tau] < threshold) {
            // 局所最小値まで進む
            while (tau + 1 < n && cmdf[tau + 1] < cmdf[tau])
                ++tau;
            return tau;
        }
    }
    return -1; // 見つからない
}

// ステップ5: 放物線補間で tau を精緻化
static double parabolicInterpolation(const std::vector<double>& cmdf, int tau)
{
    int n = static_cast<int>(cmdf.size());
    if (tau <= 0 || tau >= n - 1) return static_cast<double>(tau);
    double s0 = cmdf[tau - 1];
    double s1 = cmdf[tau];
    double s2 = cmdf[tau + 1];
    double denom = 2.0 * (s0 - 2.0 * s1 + s2);
    if (std::abs(denom) < 1e-12) return static_cast<double>(tau);
    return tau + (s0 - s2) / denom;
}

} // namespace yin_detail

// ============================================================
// 公開インターフェース
// ============================================================

// WAV の先頭部分から F0 を推定する。
// sampleRate: 入力 PCM のサンプリング周波数
// analyzeMs : 解析に使う長さ [ms]（デフォルト 200ms, 長いほど安定）
inline PitchResult estimatePitch(
    const std::vector<int16_t>& pcm,
    uint32_t sampleRate,
    double   analyzeMs   = 200.0,
    double   yinThresh   = 0.10)   // YIN 内部閾値（小さいほど厳しい）
{
    // 解析に使うサンプル数
    const int totalSamples = static_cast<int>(pcm.size());
    const int analyzeSamples = std::min(
        totalSamples,
        static_cast<int>(sampleRate * analyzeMs / 1000.0));

    if (analyzeSamples < 256) return {}; // データ不足

    // F0 探索範囲を tau (サンプル数) に変換
    // 50 Hz〜2000 Hz → tau_max〜tau_min
    const int tauMin = std::max(1, static_cast<int>(sampleRate / 2000.0));
    const int tauMax = static_cast<int>(sampleRate / 50.0);

    // フレームサイズ = analyzeSamples - tauMax（オーバーラップ分を確保）
    const int frameSize = analyzeSamples - tauMax;
    if (frameSize <= 0) return {};

    // YIN 本体
    auto df    = yin_detail::differenceFunction(pcm.data(), frameSize, tauMax);
    auto cmdf  = yin_detail::cmndf(df);

    // tauMin 未満を無効化
    for (int i = 0; i < tauMin && i < static_cast<int>(cmdf.size()); ++i)
        cmdf[i] = 1.0;

    int bestTau = yin_detail::absoluteThreshold(cmdf, yinThresh);

    // 見つからない場合は cmdf 全体の最小値を使う（低信頼度）
    if (bestTau < 0) {
        bestTau = static_cast<int>(
            std::min_element(cmdf.begin() + tauMin, cmdf.end()) - cmdf.begin());
    }

    if (bestTau <= 0 || bestTau >= static_cast<int>(cmdf.size())) return {};

    // 放物線補間で精緻化
    double refinedTau = yin_detail::parabolicInterpolation(cmdf, bestTau);
    if (refinedTau <= 0.0) return {};

    float freq = static_cast<float>(sampleRate / refinedTau);

    // 信頼度: CMNDF 値が低いほど信頼度が高い（1 - cmdf[tau] で正規化）
    double cmdfVal = cmdf[bestTau];
    float  conf    = static_cast<float>(std::max(0.0, std::min(1.0, 1.0 - cmdfVal)));

    return { freq, conf };
}

// ============================================================
// 周波数 → MIDI ノート番号変換
// A4 = 440 Hz = MIDI note 69
// ============================================================
inline int freqToMidiNote(float freqHz)
{
    if (freqHz <= 0.0f) return 69;
    return static_cast<int>(std::round(69.0f + 12.0f * std::log2(freqHz / 440.0f)));
}

// MIDI ノート番号 → オクターブ番号 (C4=60 → octave 4)
inline int midiNoteToOctave(int note)
{
    return (note / 12) - 1;
}

// ============================================================
// オクターブ制約への正規化
//
// octave >= 0 : C<octave>〜B<octave> の範囲 (12音) に丸める
// octave  < 0 : 制約なし (MIDI 0〜127 にクランプするだけ)
// ============================================================
inline int normalizeToOctave(int midiNote, int octave)
{
    midiNote = std::max(0, std::min(127, midiNote));
    if (octave < 0) return midiNote;

    // 対象オクターブの基準ノート (C)
    const int baseNote = (octave + 1) * 12; // C-1=0, C0=12, C4=60
    // 音名 (0〜11) を保ちつつオクターブだけ変える
    const int pitchClass = ((midiNote % 12) + 12) % 12;
    int result = baseNote + pitchClass;

    // MIDI 範囲クランプ
    return std::max(0, std::min(127, result));
}
