#pragma once
#include "pitch_estimator.h"
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>

// ============================================================
// サステイン楽器音を対象としたループポイント自動検出
//
// 方針: WAV に smpl チャンクが無いサンプル向けのフォールバック。
// estimatePitch() (YIN) で求めた基本周期の整数倍をループ長の候補とし、
// 接続点 (loop_end から loop_start へジャンプする瞬間) の値・傾きの
// 連続性が最も良い候補を探索する。和音・打楽器・SE 等の非周期音は
// ピッチ推定の信頼度不足として検出失敗を返す (root_note "auto" と同じ
// フォールバック思想)。
// ============================================================

struct LoopDetectResult {
    bool     found       = false;
    uint32_t startSample = 0; // 包含
    uint32_t endSample   = 0; // 包含
    float    confidence  = 0.0f;
};

// この値未満は「検出失敗」としてフォールバックする
static constexpr float LOOP_CONFIDENCE_THRESHOLD = 0.5f;

inline LoopDetectResult detectLoopPoints(
    const std::vector<int16_t>& pcm,
    uint32_t sampleRate)
{
    LoopDetectResult result{};

    const int totalSamples = static_cast<int>(pcm.size());
    if (totalSamples < 256) return result;

    // --- アタック(立ち上がり)区間をスキップしてサステイン領域を解析 ---
    // 50ms または全体の1/4の小さい方をアタックとみなす
    const int attackSkip = std::min(
        totalSamples / 4,
        static_cast<int>(sampleRate * 0.05));

    std::vector<int16_t> sustain(pcm.begin() + attackSkip, pcm.end());
    PitchResult pitch = estimatePitch(sustain, sampleRate);
    if (pitch.frequency <= 0.0f || pitch.confidence < YIN_CONFIDENCE_THRESHOLD)
        return result; // 非周期音 → 検出失敗

    const int period = static_cast<int>(std::round(sampleRate / pitch.frequency));
    if (period < 2) return result;

    // --- ループ長の下限: 最低4周期、かつ50ms以上 ---
    const int minPeriods = std::max(4, static_cast<int>(
        std::ceil(sampleRate * 0.05 / period)));

    // --- 探索範囲: アタック直後から4周期分をループ開始候補とする ---
    const int searchStart = attackSkip;
    const int searchEnd   = std::min(totalSamples - 1, attackSkip + period * 4);

    double bestScore = -1.0;
    int    bestStart = -1, bestEnd = -1;

    for (int s = searchStart; s <= searchEnd; ++s) {
        const int maxK = std::min(64, (totalSamples - 1 - s) / period);
        for (int k = minPeriods; k <= maxK; ++k) {
            const int e = s + k * period;
            if (e <= s || e >= totalSamples - 1) continue;

            // 接続点の値の連続性 + 傾きの連続性を誤差として評価する
            const double valueErr = static_cast<double>(pcm[e]) - pcm[s];
            const double slopeA   = static_cast<double>(pcm[e]) - pcm[e - 1];
            const double slopeB   = static_cast<double>(pcm[s + 1]) - pcm[s];
            const double slopeErr = slopeA - slopeB;

            const double score = valueErr * valueErr + slopeErr * slopeErr;
            if (bestScore < 0.0 || score < bestScore) {
                bestScore = score;
                bestStart = s;
                bestEnd   = e;
            }
        }
    }

    if (bestStart < 0) return result; // 十分なサステイン長が無い

    // --- スコア → 信頼度への変換 (16bit フルスケール基準のヒューリスティック) ---
    const double fullScale     = 32768.0;
    const double normalizedErr = std::sqrt(bestScore) / fullScale;
    const float  confidence    = static_cast<float>(
        std::max(0.0, std::min(1.0, 1.0 - normalizedErr * 8.0)));

    if (confidence < LOOP_CONFIDENCE_THRESHOLD) return result; // 検出失敗

    result.found       = true;
    result.startSample = static_cast<uint32_t>(bestStart);
    result.endSample   = static_cast<uint32_t>(bestEnd);
    result.confidence  = confidence;
    return result;
}
