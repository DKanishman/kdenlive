/*
    SPDX-FileCopyrightText: 2012 Simon Andreas Eugster <simon.eu@gmail.com>
    This file is part of kdenlive. See www.kdenlive.org.

SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "audioEnvelope.h"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "kdenlive_debug.h"
#include <QImage>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>

AudioEnvelope::AudioEnvelope(const QString &binId, int clipId, std::pair<int, int> stream, size_t offset, size_t length, size_t startPos)
    : m_offset(offset)
    , m_clipId(clipId)
    , m_startpos(startPos)
{
    connect(&m_watcher, &QFutureWatcherBase::finished, this, [this] { Q_EMIT envelopeReady(this); });
    const auto model = pCore ? pCore->projectItemModel() : nullptr;
    const auto clip = model ? model->getClipByBinID(binId) : nullptr;
    if (!clip || !clip->statusReady()) {
        m_inputError = QStringLiteral("The audio source is missing or not ready.");
        return;
    }
    auto source = clip->cloneProducer();
    if (!source || !source->is_valid() || !clip->hasAudio()) {
        m_inputError = QStringLiteral("The source has no decodable audio producer.");
        return;
    }
    if ((stream.first == -1 && stream.second != -1) || stream.first < -1 || stream.second < -1 ||
        (stream.first >= 0 && (!clip->audioStreams().contains(stream.first) || clip->getAudioStreamFfmpegIndex(stream.first) != stream.second))) {
        m_inputError = QStringLiteral("The selected native audio stream does not exist or its indices disagree.");
        return;
    }
    const size_t available = size_t(qMax(0, source->get_playtime()));
    if (offset >= available || (length && length > available - offset)) {
        m_inputError = QStringLiteral("The requested audio range is empty or exceeds the analysis frame limit.");
        return;
    }
    const size_t selected = length ? length : available - offset;
    if (selected > 1000000) {
        m_inputError = QStringLiteral("The requested audio range exceeds the analysis frame limit.");
        return;
    }
    // Always analyze the exact requested source window, including short clips.
    // A native cut maps its frame zero to the requested source-in.
    m_producer.reset(source->cut(int(offset), int(offset + selected - 1)));
    m_offset = 0;
    if (!m_producer || !m_producer->is_valid()) {
        m_inputError = QStringLiteral("The native audio source cut could not be created.");
        return;
    }
    m_envelopeSize = selected;
    m_producer->set("set.test_image", 1);
    if (stream.first > -1) {
        m_producer->parent().set("audio_index", stream.first);
        m_producer->parent().set("astream", stream.second);
    }
}

AudioEnvelope::~AudioEnvelope()
{
    disconnect(&m_watcher, nullptr, this, nullptr);
    cancel();
    if (m_started) {
        m_audioSummary.waitForFinished();
        m_watcher.waitForFinished();
    }
}

void AudioEnvelope::startComputeEnvelope()
{
    if (m_started) return;
    m_started = true;
    m_audioSummary = QtConcurrent::run(&AudioEnvelope::loadAndNormalizeEnvelope, this);
    m_watcher.setFuture(m_audioSummary);
}

bool AudioEnvelope::hasComputationStarted() const
{
    return m_started;
}
void AudioEnvelope::cancel()
{
    m_cancelled.store(true);
}
bool AudioEnvelope::isFinished() const
{
    return m_started && m_audioSummary.isFinished();
}
bool AudioEnvelope::wasCancelled() const
{
    return m_cancelled.load();
}
QString AudioEnvelope::error()
{
    return audioSummary().error;
}

const AudioEnvelope::AudioSummary &AudioEnvelope::audioSummary()
{
    Q_ASSERT(hasComputationStarted());
    m_audioSummary.waitForFinished();
    Q_ASSERT(m_audioSummary.constBegin() != m_audioSummary.constEnd());
    // We use this instead of m_audioSummary.result() in order to return
    // a const reference instead of a copy.
    return *m_audioSummary.constBegin();
}

size_t AudioEnvelope::offset()
{
    return m_offset;
}

const std::vector<qint64> &AudioEnvelope::envelope()
{
    // Blocks until the summary is available.
    return audioSummary().audioAmplitudes;
}

AudioEnvelope::AudioSummary AudioEnvelope::loadAndNormalizeEnvelope() const
{
    AudioSummary summary;
    if (!m_inputError.isEmpty()) {
        summary.error = m_inputError;
        return summary;
    }
    // MLT cuts own their range but do not inherit the parent's stream metadata.
    // Decode the already validated source instead of probing metadata on the cut.
    if (!m_producer || !m_producer->is_valid() || !m_envelopeSize) {
        summary.error = QStringLiteral("The selected source has no audio analysis data.");
        return summary;
    }
    summary.audioAmplitudes.resize(m_envelopeSize);
    for (size_t i = 0; i < m_envelopeSize; ++i) {
        if (m_cancelled.load()) {
            summary.audioAmplitudes.clear();
            summary.error = QStringLiteral("Audio analysis cancelled.");
            return summary;
        }
        m_producer->seek(int(i));
        std::unique_ptr<Mlt::Frame> frame(m_producer->get_frame());
        if (!frame || !frame->is_valid()) {
            summary.error = QStringLiteral("Native audio frame decoding failed.");
            break;
        }
        mlt_audio_format format = mlt_audio_s16;
        int rate = 48000, channels = 1;
        int samples = mlt_audio_calculate_frame_samples(float(m_producer->get_fps()), rate, int(i));
        const auto *data = static_cast<const qint16 *>(frame->get_audio(format, rate, channels, samples));
        if (!data || format != mlt_audio_s16 || rate <= 0 || samples <= 0 || samples > 384000 || channels <= 0 || channels > 64 || frame->get_int("test_audio")) {
            summary.error = QStringLiteral("Native audio decoding returned an invalid sample buffer.");
            break;
        }
        qint64 amplitude = 0;
        for (qsizetype sample = 0; sample < qsizetype(samples) * channels; ++sample)
            amplitude += std::abs(int(data[sample]));
        summary.audioAmplitudes[i] = amplitude / channels;
        if (i % 25 == 0) Q_EMIT const_cast<AudioEnvelope *>(this)->progress(int(100 * i / m_envelopeSize));
    }
    if (!summary.error.isEmpty()) {
        summary.audioAmplitudes.clear();
        return summary;
    }
    long double total = 0;
    for (const auto value : summary.audioAmplitudes)
        total += value;
    const qint64 mean = qint64(total / summary.audioAmplitudes.size());
    for (auto &value : summary.audioAmplitudes) {
        value -= mean;
        summary.amplitudeMax = std::max(summary.amplitudeMax, qAbs(value));
    }
    if (!summary.amplitudeMax) {
        summary.error = QStringLiteral("The selected audio is silent or has no varying alignment evidence.");
        summary.audioAmplitudes.clear();
    }
    return summary;
}

int AudioEnvelope::clipId() const
{
    return m_clipId;
}

size_t AudioEnvelope::startPos() const
{
    return m_startpos;
}

QImage AudioEnvelope::drawEnvelope()
{
    const AudioSummary &summary = audioSummary();

    QImage img(int(m_envelopeSize), 400, QImage::Format_ARGB32);
    img.fill(qRgb(255, 255, 255));

    if (summary.amplitudeMax == 0) {
        return img;
    }

    for (int x = 0; x < img.width(); ++x) {
        double fy = double(summary.audioAmplitudes[size_t(x)]) / summary.amplitudeMax * img.height();
        for (int y = img.height() - 1; y > img.height() - 1 - fy; --y) {
            img.setPixel(x, y, qRgb(50, 50, 50));
        }
    }
    return img;
}

void AudioEnvelope::dumpInfo()
{
    if (!m_audioSummary.isFinished()) {
        qCDebug(KDENLIVE_LOG) << "Envelope not yet generated, no information available.";
    } else {
        const AudioSummary &summary = audioSummary();
        qCDebug(KDENLIVE_LOG) << "Envelope info"
                              << "\n* size = " << summary.audioAmplitudes.size() << "\n* max = " << summary.amplitudeMax;
    }
}
