/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#pragma once

#include <QByteArray>
#include <QSet>
#include <QString>
#include <memory>
#include <mlt++/MltFrame.h>
#include <mlt++/MltProducer.h>
#include <mlt++/MltProfile.h>

namespace McpRender {
// Container duration can be correct even when an encoder silently drops a stream.
// Validate requested stream kinds and bounded real decoded samples before publication.
inline bool verifyStreams(Mlt::Producer &rendered, Mlt::Profile &profile, bool requireVideo, bool requireAudio, QString &reason)
{
    if (!rendered.is_valid()) {
        reason = QStringLiteral("Native rendered media cannot be opened.");
        return false;
    }
    const int frames = rendered.get_length();
    bool hasVideo = false, hasAudio = false;
    const int streams = qBound(0, rendered.get_int("meta.media.nb_streams"), 128);
    for (int index = 0; index < streams; ++index) {
        const auto property = QStringLiteral("meta.media.%1.stream.type").arg(index).toUtf8();
        const QByteArray type(rendered.get(property.constData()));
        hasVideo |= type == "video";
        hasAudio |= type == "audio";
    }
    if ((requireVideo && (!hasVideo || rendered.get_int("video_index") < 0)) ||
        (requireAudio && (!hasAudio || rendered.get_int("audio_index") < 0))) {
        reason = QStringLiteral("Native rendered media is missing a requested video or audio stream.");
        return false;
    }
    // Decode bounded samples instead of accepting container duration alone.
    for (const int position : QSet<int>{0, frames / 2, qMax(0, frames - 1)}) {
        rendered.seek(position);
        std::unique_ptr<Mlt::Frame> frame(rendered.get_frame());
        if (!frame || !frame->is_valid()) {
            reason = QStringLiteral("Native rendered media did not produce a decodable frame.");
            return false;
        }
        if (requireVideo) {
            mlt_image_format format = mlt_image_rgb;
            int width = 32, height = 32;
            if (!frame->get_image(format, width, height) || frame->get_int("test_image")) {
                reason = QStringLiteral("Native rendered video samples cannot be decoded.");
                return false;
            }
        }
        if (requireAudio) {
            mlt_audio_format format = mlt_audio_f32le;
            int frequency = 48000, channels = 2;
            int samples = mlt_audio_calculate_frame_samples(profile.fps(), frequency, position);
            if (!frame->get_audio(format, frequency, channels, samples) || frame->get_int("test_audio")) {
                reason = QStringLiteral("Native rendered audio samples cannot be decoded.");
                return false;
            }
        }
    }
    return true;
}
} // namespace McpRender
