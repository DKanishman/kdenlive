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
    McpProject project;
    project.open();
    const auto bin = pCore->projectItemModel();
    const QString id = KdenliveTests::createProducerWithSound(pCore->getProjectProfile(), bin, 300);
    const auto source = bin->getClipByBinID(id);
    REQUIRE(source);
    source->setProducerProperty(QStringLiteral("timecode"), QStringLiteral("10000"));
    CHECK(source->getStartTCFromProperties(0) == 250);
    source->resetProducerProperty(QStringLiteral("timecode"));
    source->setProducerProperty(QStringLiteral("meta.attr.time_reference.markup"), QStringLiteral("480000"));
    source->setProducerProperty(QStringLiteral("meta.media.0.codec.sample_rate"), QStringLiteral("48000"));
    CHECK(source->getStartTCFromProperties(0) == 250);
    source->setProducerProperty(QStringLiteral("meta.attr.time_reference.markup"), QStringLiteral("0"));
    CHECK(source->getStartTCFromProperties(0) == 0);
    CHECK(source->hasTimecode());
    source->setProducerProperty(QStringLiteral("meta.media.0.codec.sample_rate"), QStringLiteral("0"));
    CHECK(source->getStartTCFromProperties(0) == -1);
    source->resetProducerProperty(QStringLiteral("meta.media.0.codec.sample_rate"));
    CHECK(source->getStartTCFromProperties(0) == -1);
    source->setProducerProperty(QStringLiteral("meta.attr.time_reference.markup"), QStringLiteral("9223372036854775807"));
    source->setProducerProperty(QStringLiteral("meta.media.0.codec.sample_rate"), QStringLiteral("1"));
    CHECK(source->getStartTCFromProperties(0) == -1);
}
