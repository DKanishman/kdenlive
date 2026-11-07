/*
SPDX-FileCopyrightText: 2012 Simon A. Eugster (Granjow)  <simon.eu@gmail.com>
SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "audioCorrelation.h"
#include "fftCorrelation.h"

#include "kdenlive_debug.h"
#include "klocalizedstring.h"
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

AudioCorrelation::AudioCorrelation(std::unique_ptr<AudioEnvelope> mainTrackEnvelope)
    : m_mainTrackEnvelope(std::move(mainTrackEnvelope))
{
    // Q_ASSERT(!mainTrackEnvelope->hasComputationStarted());
    connect(m_mainTrackEnvelope.get(), &AudioEnvelope::envelopeReady, this, &AudioCorrelation::slotAnnounceEnvelope);
    m_mainTrackEnvelope->startComputeEnvelope();
}

AudioCorrelation::~AudioCorrelation()
{
    cancel();
    for (AudioEnvelope *envelope : std::as_const(m_children)) {
        delete envelope;
    }
    for (AudioCorrelationInfo *info : std::as_const(m_correlations)) {
        delete info;
    }

    qCDebug(KDENLIVE_LOG) << "Envelope deleted.";
}

void AudioCorrelation::cancel()
{
    m_cancelled = true;
    m_mainTrackEnvelope->cancel();
    for (auto *envelope : std::as_const(m_children))
        envelope->cancel();
}

void AudioCorrelation::slotAnnounceEnvelope()
{
    const QString error = m_mainTrackEnvelope->error();
    Q_EMIT displayMessage(error.isEmpty() ? i18n("Audio analysis finished") : error, error.isEmpty() ? OperationCompletedMessage : ErrorMessage, 300);
    for (auto *envelope : std::as_const(m_children))
        if (envelope->isFinished()) slotProcessChild(envelope);
}

void AudioCorrelation::addChild(AudioEnvelope *envelope)
{
    Q_ASSERT(envelope && !envelope->hasComputationStarted());
    if (!envelope) return;
    // Own running children immediately; otherwise closing/changing the
    // reference can orphan a worker whose producer outlives the editor.
    envelope->setParent(this);
    m_children.append(envelope);
    m_correlations.append(nullptr);
    connect(envelope, &AudioEnvelope::envelopeReady, this, &AudioCorrelation::slotProcessChild);
    if (m_cancelled) envelope->cancel();
    envelope->startComputeEnvelope();
}

void AudioCorrelation::slotProcessChild(AudioEnvelope *envelope)
{
    if (!envelope || m_processed.contains(envelope) || !m_mainTrackEnvelope->isFinished()) return;
    m_processed.insert(envelope);
    const int index = m_children.indexOf(envelope);
    if (index < 0) return;
    if (m_cancelled || envelope->wasCancelled() || m_mainTrackEnvelope->wasCancelled()) {
        Q_EMIT alignmentCancelled(envelope->clipId());
        return;
    }
    const QString error = !m_mainTrackEnvelope->error().isEmpty() ? m_mainTrackEnvelope->error() : envelope->error();
    if (!error.isEmpty()) {
        Q_EMIT displayMessage(error, ErrorMessage, 500);
        Q_EMIT alignmentFailed(envelope->clipId(), error);
        return;
    }
    const auto &envMain = m_mainTrackEnvelope->envelope();
    const auto &envSub = envelope->envelope();
    if (envMain.empty() || envSub.empty()) {
        Q_EMIT alignmentFailed(envelope->clipId(), QStringLiteral("The audio envelope is empty."));
        return;
    }
    auto *info = new AudioCorrelationInfo(envMain.size(), envSub.size());
    qint64 maximum = 0;
    if (envSub.size() > 200)
        FFTCorrelation::correlate(envMain.data(), envMain.size(), envSub.data(), envSub.size(), info->correlationVector());
    else {
        correlate(envMain.data(), envMain.size(), envSub.data(), envSub.size(), info->correlationVector(), &maximum);
        info->setMax(maximum);
    }
    m_correlations[index] = info;
    Q_EMIT gotAudioAlignData(envelope->clipId(), getShift(index));
}

int AudioCorrelation::getShift(int childIndex) const
{
    Q_ASSERT(childIndex >= 0);
    Q_ASSERT(childIndex < m_correlations.size());

    Q_ASSERT(m_correlations.at(childIndex));
    const qint64 offset =
        qint64(m_correlations.at(childIndex)->maxIndex()) - qint64(m_children.at(childIndex)->envelope().size()) + qint64(m_children.at(childIndex)->offset());
    return offset < std::numeric_limits<int>::min() || offset > std::numeric_limits<int>::max() ? 0 : int(offset);
}

AudioCorrelationInfo const *AudioCorrelation::info(int childIndex) const
{
    Q_ASSERT(childIndex >= 0);
    Q_ASSERT(childIndex < m_correlations.size());

    return m_correlations.at(childIndex);
}

void AudioCorrelation::correlate(const qint64 *envMain, size_t sizeMain, const qint64 *envSub, size_t sizeSub, qint64 *correlation, qint64 *out_max)
{
    Q_ASSERT(correlation != nullptr);
    if (out_max) *out_max = 0;
    // Empty input has no alignment evidence. Initialize every advertised output
    // element, including the zero-overlap endpoints, before returning.
    const size_t maxIndex = size_t(std::numeric_limits<qint64>::max());
    if (sizeMain > maxIndex || sizeSub > maxIndex || sizeMain >= maxIndex - sizeSub) return;
    std::fill(correlation, correlation + sizeMain + sizeSub + 1, qint64(0));
    if (!sizeMain || !sizeSub || !envMain || !envSub) return;
    const qint64 mainSize = qint64(sizeMain), subSize = qint64(sizeSub);
    long double largestMain = 0, largestSub = 0;
    for (size_t i = 0; i < sizeMain; ++i)
        largestMain = std::max(largestMain, std::abs(static_cast<long double>(envMain[i])));
    for (size_t i = 0; i < sizeSub; ++i)
        largestSub = std::max(largestSub, std::abs(static_cast<long double>(envSub[i])));
    if (largestMain == 0 || largestSub == 0) return;
    // Preserve ordinary integer results, but scale values before products that
    // could overflow. The scale is constant across lags, preserving peak order.
    const long double worst = largestMain * largestSub * static_cast<long double>(std::min(sizeMain, sizeSub));
    const long double scale = worst > static_cast<long double>(std::numeric_limits<qint64>::max()) / 4
                                  ? (static_cast<long double>(std::numeric_limits<qint64>::max()) / 4) / worst
                                  : 1;
    qint64 maximum = 0;
    // Lags are signed. An unsigned -sizeSub starts beyond sizeMain and leaves
    // the whole vector uninitialized for the native short-envelope branch.
    for (qint64 shift = -subSize; shift <= mainSize; ++shift) {
        const qint64 mainStart = std::max(qint64(0), shift);
        const qint64 subStart = std::max(qint64(0), -shift);
        const qint64 count = std::min(mainSize - mainStart, subSize - subStart);
        long double sum = 0;
        for (qint64 i = 0; i < count; ++i)
            sum += static_cast<long double>(envMain[size_t(mainStart + i)]) * static_cast<long double>(envSub[size_t(subStart + i)]) * scale;
        const qint64 value = qint64(std::llround(sum));
        correlation[size_t(subSize + shift)] = value;
        maximum = std::max(maximum, value);
    }
    if (out_max) *out_max = maximum;
}
