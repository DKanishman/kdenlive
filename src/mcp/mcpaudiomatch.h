/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#pragma once

#include "lib/audio/fftCorrelation.h"
#include <QString>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace McpAudioMatch {
struct Result
{
    bool ok{false};
    bool cancelled{false};
    int lag{0};
    int overlap{0};
    double correlation{0};
    double runnerUp{0};
    bool weak{true};
    QString error;
};

// FFT scores are scaled convolutions, not confidence values. Compute Pearson
// coefficients over each real overlap, then independently rescore the winners.
inline Result match(const std::vector<qint64> &referenceValues, const std::vector<qint64> &targetValues, int maxShift, int minimumOverlap,
                    const std::shared_ptr<std::atomic_bool> &cancelled)
{
    Result result;
    if (referenceValues.size() < 8 || targetValues.size() < 8 || referenceValues.size() > 30000 || targetValues.size() > 30000 || minimumOverlap < 8 ||
        minimumOverlap > int(std::min(referenceValues.size(), targetValues.size())) || maxShift < 0 || maxShift > 30000) {
        result.error = QStringLiteral("Audio matching requires 8-30000 frames, a valid overlap and a bounded lag search.");
        return result;
    }
    const auto stopped = [&] { return cancelled && cancelled->load(); };
    if (stopped()) {
        result.cancelled = true;
        return result;
    }
    const auto validSamples = [](const std::vector<qint64> &values) {
        return std::all_of(values.begin(), values.end(), [](qint64 value) { return value >= 0 && value <= 32768; });
    };
    if (!validSamples(referenceValues) || !validSamples(targetValues)) {
        result.error = QStringLiteral("Expected bounded mean-absolute signed-16 PCM amplitudes.");
        return result;
    }
    // Remove the DC component before the single-precision FFT. Without this,
    // nearly constant full-scale envelopes lose their small, useful variation
    // when large products are subtracted during normalization.
    const auto center = [](const std::vector<qint64> &values) {
        qint64 sum = 0;
        for (auto value : values)
            sum += value;
        const qint64 mean = sum / qint64(values.size());
        auto centered = values;
        for (auto &value : centered)
            value -= mean;
        return centered;
    };
    const auto reference = center(referenceValues), target = center(targetValues);
    struct Prefix
    {
        std::vector<long double> sum, square;
        explicit Prefix(const std::vector<qint64> &values)
            : sum(values.size() + 1)
            , square(values.size() + 1)
        {
            for (size_t i = 0; i < values.size(); ++i) {
                sum[i + 1] = sum[i] + values[i];
                square[i + 1] = square[i] + static_cast<long double>(values[i]) * values[i];
            }
        }
    };
    const Prefix a(reference), b(target);
    const auto variance = [](const Prefix &p, int begin, int count) {
        const auto sum = p.sum[begin + count] - p.sum[begin];
        return p.square[begin + count] - p.square[begin] - sum * sum / count;
    };
    if (variance(a, 0, int(reference.size())) <= 0 || variance(b, 0, int(target.size())) <= 0) {
        result.error = QStringLiteral("Silent or constant audio has no varying alignment evidence.");
        return result;
    }
    const bool fft = std::max(reference.size(), target.size()) > 200;
    std::vector<float> convolution;
    long double fftScale = 1;
    if (fft) {
        convolution.resize(reference.size() + target.size() + 1);
        FFTCorrelation::correlate(reference.data(), reference.size(), target.data(), target.size(), convolution.data());
        size_t length = 64;
        while (length / 2 < std::max(reference.size(), target.size()))
            length *= 2;
        long double maxA = 1, maxB = 1;
        for (auto value : reference)
            maxA = std::max(maxA, std::abs(static_cast<long double>(value)));
        for (auto value : target)
            maxB = std::max(maxB, std::abs(static_cast<long double>(value)));
        fftScale = maxA * maxB / length;
    }
    struct Score
    {
        int lag;
        int overlap;
        double value;
    };
    std::vector<Score> scores;
    for (int lag = std::max(-maxShift, -int(target.size()) + minimumOverlap); lag <= std::min(maxShift, int(reference.size()) - minimumOverlap); ++lag) {
        if (stopped()) {
            result.cancelled = true;
            return result;
        }
        const int startA = std::max(0, lag), startB = std::max(0, -lag);
        const int count = std::min(int(reference.size()) - startA, int(target.size()) - startB);
        const auto va = variance(a, startA, count), vb = variance(b, startB, count);
        if (va <= 0 || vb <= 0) continue;
        long double cross = 0;
        if (fft)
            cross = convolution[int(target.size()) + lag] * fftScale;
        else
            for (int i = 0; i < count; ++i)
                cross += static_cast<long double>(reference[startA + i]) * target[startB + i];
        const auto sa = a.sum[startA + count] - a.sum[startA], sb = b.sum[startB + count] - b.sum[startB];
        const double coefficient = double((cross - sa * sb / count) / std::sqrt(va * vb));
        if (std::isfinite(coefficient)) scores.push_back({lag, count, std::clamp(coefficient, -1.0, 1.0)});
    }
    if (scores.empty()) {
        result.error = QStringLiteral("No nonconstant audio overlap meets the requested constraints.");
        return result;
    }
    // Directly recompute a bounded set around the FFT candidates. This avoids
    // publishing a confidence value inferred from FFT amplitude scaling.
    std::stable_sort(scores.begin(), scores.end(), [](const Score &x, const Score &y) { return x.value > y.value; });
    const bool uncertainRanking = fft && scores.size() > 64 && scores[63].value > scores.front().value - .1;
    if (fft) {
        scores.resize(std::min<size_t>(scores.size(), 64));
        for (auto &score : scores) {
            if (stopped()) {
                result.cancelled = true;
                return result;
            }
            const int sa = std::max(0, score.lag), sb = std::max(0, -score.lag), count = score.overlap;
            long double cross = 0;
            for (int i = 0; i < count; ++i)
                cross += static_cast<long double>(reference[sa + i]) * target[sb + i];
            const auto sumA = a.sum[sa + count] - a.sum[sa], sumB = b.sum[sb + count] - b.sum[sb];
            score.value = std::clamp(double((cross - sumA * sumB / count) / std::sqrt(variance(a, sa, count) * variance(b, sb, count))), -1.0, 1.0);
        }
        std::stable_sort(scores.begin(), scores.end(), [](const Score &x, const Score &y) { return x.value > y.value; });
    }
    const auto peak = scores.front();
    result.lag = peak.lag;
    result.overlap = peak.overlap;
    result.correlation = peak.value;
    for (const auto &score : scores)
        if (std::abs(score.lag - peak.lag) > 1) result.runnerUp = std::max(result.runnerUp, score.value);
    result.weak = uncertainRanking || peak.value < .65 || peak.value - result.runnerUp < .08;
    result.ok = true;
    return result;
}
} // namespace McpAudioMatch
