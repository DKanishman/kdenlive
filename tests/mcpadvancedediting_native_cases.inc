/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "lib/audio/audioCorrelation.h"
#include <algorithm>
#include <vector>

TEST_CASE("Native audio correlation initializes short positive negative and empty lags", "[MCP][AdvancedEditing][alignment][native]")
{
    const std::vector<qint64> reference{0, 2, 5, 1, 0};
    const std::vector<qint64> child{2, 5, 1};
    std::vector<qint64> result(reference.size() + child.size() + 1, -999);
    qint64 maximum = -1;
    AudioCorrelation::correlate(reference.data(), reference.size(), child.data(), child.size(), result.data(), &maximum);
    CHECK(maximum == 30);
    CHECK(std::max_element(result.begin(), result.end()) - result.begin() - child.size() == 1);
    for (qint64 lag = -qint64(child.size()); lag <= qint64(reference.size()); ++lag) {
        qint64 expected = 0;
        for (qint64 i = 0; i < qint64(child.size()); ++i)
            if (i + lag >= 0 && i + lag < qint64(reference.size())) expected += child[size_t(i)] * reference[size_t(i + lag)];
        CHECK(result[size_t(qint64(child.size()) + lag)] == expected);
    }
    AudioCorrelation::correlate(child.data(), child.size(), reference.data(), reference.size(), result.data(), &maximum);
    CHECK(std::max_element(result.begin(), result.end()) - result.begin() - reference.size() == -1);
    std::fill(result.begin(), result.end(), qint64(-999));
    AudioCorrelation::correlate(nullptr, 0, child.data(), child.size(), result.data(), &maximum);
    CHECK(maximum == 0);
    for (size_t i = 0; i <= child.size(); ++i)
        CHECK(result[i] == 0);
}

TEST_CASE("Native audio correlation keeps peak order when raw products exceed int64", "[MCP][AdvancedEditing][alignment][native][bounds]")
{
    const std::vector<qint64> reference{0, 1000000000000LL, 4000000000000LL, 2000000000000LL, 0};
    const std::vector<qint64> child{1000000000000LL, 4000000000000LL, 2000000000000LL};
    std::vector<qint64> result(reference.size() + child.size() + 1, -999);
    qint64 maximum = 0;
    AudioCorrelation::correlate(reference.data(), reference.size(), child.data(), child.size(), result.data(), &maximum);
    REQUIRE(maximum > 0);
    CHECK(std::max_element(result.begin(), result.end()) - result.begin() - child.size() == 1);
}

TEST_CASE("Native timecode converts milliseconds and Broadcast Wave samples into project frames", "[MCP][AdvancedEditing][alignment][timecode]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    McpProject project;
    project.open();
    const auto bin = pCore->projectItemModel();
    const QString id = importFixture(project, wavFixture(directory));
    const auto source = bin->getClipByBinID(id);
    REQUIRE(source);
    source->setProducerProperty(QStringLiteral("timecode"), QStringLiteral("10000"));
    CHECK(source->getStartTCFromProperties(0) == 250);
    // Old projects may contain this stale cache in milliseconds.
    source->setProducerProperty(QStringLiteral("kdenlive:record_start_frame"), QStringLiteral("10000"));
    CHECK(source->getStartTimecode(0) == 250);
    source->setProducerProperty(QStringLiteral("timecode"), QStringLiteral("20000"));
    CHECK(source->getStartTimecode(0) == 500);
    REQUIRE(pCore->setCurrentProfile(QStringLiteral("atsc_1080p_2997")));
    CHECK(source->getStartTimecode(0) == 599);
    REQUIRE(pCore->setCurrentProfile(QStringLiteral("dv_pal")));
    source->resetProducerProperty(QStringLiteral("timecode"));
    source->setProducerProperty(QStringLiteral("meta.attr.time_reference.markup"), QStringLiteral("480000"));
    source->setProducerProperty(QStringLiteral("meta.media.0.codec.sample_rate"), QStringLiteral("48000"));
    CHECK(source->getStartTCFromProperties(0) == 250);
    source->setProducerProperty(QStringLiteral("meta.attr.time_reference.markup"), QStringLiteral("0"));
    CHECK(source->getStartTCFromProperties(0) == 0);
    CHECK(source->hasTimecode());
    source->setProducerProperty(QStringLiteral("meta.media.0.codec.sample_rate"), QStringLiteral("0"));
    CHECK(source->getStartTCFromProperties(0) == -1);
    CHECK_FALSE(source->hasTimecode());
    source->resetProducerProperty(QStringLiteral("meta.media.0.codec.sample_rate"));
    CHECK(source->getStartTCFromProperties(0) == -1);
    source->setProducerProperty(QStringLiteral("meta.attr.time_reference.markup"), QStringLiteral("9223372036854775807"));
    source->setProducerProperty(QStringLiteral("meta.media.0.codec.sample_rate"), QStringLiteral("1"));
    CHECK(source->getStartTCFromProperties(0) == -1);
}

namespace {
QString nativeEnvelopeFixture(const QTemporaryDir &directory, bool silence = false)
{
    const QString path = directory.filePath(silence ? QStringLiteral("envelope-silent.wav") : QStringLiteral("envelope.wav"));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    const std::vector<int> amplitudes{120, 1800, 370, 4200, 800, 2400, 410, 5000, 270, 3000};
    QByteArray pcm;
    QDataStream samples(&pcm, QIODevice::WriteOnly);
    samples.setByteOrder(QDataStream::LittleEndian);
    // Exact 25 fps frame boundaries and 48 kHz PCM avoid encoder dependencies.
    for (int frame = 0; frame < 40; ++frame) {
        const int amplitude = silence ? 0 : amplitudes[size_t(frame) % amplitudes.size()];
        for (int sample = 0; sample < 1920; ++sample) {
            samples << qint16(sample % 2 ? amplitude : -amplitude);
        }
    }
    REQUIRE(samples.status() == QDataStream::Ok);
    QDataStream header(&file);
    header.setByteOrder(QDataStream::LittleEndian);
    REQUIRE(file.write("RIFF", 4) == 4);
    header << quint32(36 + pcm.size());
    REQUIRE(file.write("WAVEfmt ", 8) == 8);
    header << quint32(16) << quint16(1) << quint16(1) << quint32(48000) << quint32(96000) << quint16(2) << quint16(16);
    REQUIRE(file.write("data", 4) == 4);
    header << quint32(pcm.size());
    REQUIRE(file.write(pcm) == pcm.size());
    REQUIRE(header.status() == QDataStream::Ok);
    file.close();
    return path;
}
}

TEST_CASE("Native audio envelopes decode exact short source windows", "[MCP][AdvancedEditing][alignment][envelope]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    McpProject project;
    project.open();
    const QString id = importFixture(project, nativeEnvelopeFixture(directory));
    const auto source = pCore->projectItemModel()->getClipByBinID(id);
    REQUIRE(source);
    const auto streams = source->audioStreams().keys();
    REQUIRE(streams.size() == 1);
    const std::pair<int, int> stream{streams.first(), 0};
    AudioEnvelope full(id, 1, stream);
    full.startComputeEnvelope();
    const auto complete = full.envelope();
    INFO(full.error().toStdString());
    REQUIRE(full.error().isEmpty());
    REQUIRE(complete.size() == 40);
    AudioEnvelope trimmed(id, 2, stream, 3, 7);
    trimmed.startComputeEnvelope();
    const auto cut = trimmed.envelope();
    INFO(trimmed.error().toStdString());
    REQUIRE(trimmed.error().isEmpty());
    REQUIRE(cut.size() == 7);
    CHECK(trimmed.offset() == 0);
    // Normalization changes the mean; pairwise differences retain exact source
    // amplitudes and prove the cut starts at frame 3 rather than source frame 0.
    for (size_t i = 0; i < cut.size(); ++i) {
        CHECK(cut[i] - cut.front() == complete[i + 3] - complete[3]);
    }
    AudioEnvelope toEnd(id, 3, stream, 33);
    toEnd.startComputeEnvelope();
    CHECK(toEnd.envelope().size() == 7);
    CHECK(toEnd.error().isEmpty());
}

TEST_CASE("Native audio envelopes fail safely for invalid ranges streams silence and cancellation", "[MCP][AdvancedEditing][alignment][envelope]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    McpProject project;
    project.open();
    const QString id = importFixture(project, nativeEnvelopeFixture(directory));
    SECTION("Missing source")
    {
        AudioEnvelope envelope(QStringLiteral("999999999"), 1);
        envelope.startComputeEnvelope();
        CHECK(envelope.envelope().empty());
        CHECK_FALSE(envelope.error().isEmpty());
    }
    SECTION("Invalid range")
    {
        AudioEnvelope envelope(id, 1, {-1, -1}, 39, 2);
        envelope.startComputeEnvelope();
        CHECK(envelope.envelope().empty());
        CHECK_FALSE(envelope.error().isEmpty());
    }
    SECTION("Invalid stream")
    {
        AudioEnvelope envelope(id, 1, {999, 999});
        envelope.startComputeEnvelope();
        CHECK(envelope.envelope().empty());
        CHECK_FALSE(envelope.error().isEmpty());
    }
    SECTION("Silent source")
    {
        const QString silent = importFixture(project, nativeEnvelopeFixture(directory, true));
        AudioEnvelope envelope(silent, 1);
        envelope.startComputeEnvelope();
        CHECK(envelope.envelope().empty());
        CHECK_FALSE(envelope.error().isEmpty());
    }
    SECTION("Cancellation before decoding")
    {
        AudioEnvelope envelope(id, 1);
        envelope.cancel();
        envelope.startComputeEnvelope();
        envelope.startComputeEnvelope();
        CHECK(envelope.envelope().empty());
        CHECK(envelope.wasCancelled());
        CHECK_FALSE(envelope.error().isEmpty());
    }
}
