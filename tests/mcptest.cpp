/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "test_utils.hpp"

#include "bin/model/markerlistmodel.hpp"
#include "effects/effectstack/model/effectitemmodel.hpp"
#include "mcp/mcpserver.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <cmath>
#include <limits>
#include <mlt++/MltFrame.h>

namespace {

// This executable uses the shared runner; enable Qt's dedicated test paths before its main().
const bool isolatedQtPaths = [] {
    QStandardPaths::setTestModeEnabled(true);
    return true;
}();

class McpProject
{
public:
    McpProject()
        : undoStack(std::make_shared<DocUndoStack>(nullptr))
    {
    }

    void open()
    {
        pCore->projectItemModel()->clean();
        pCore->setCurrentProfile(QStringLiteral("dv_pal"));
        document = std::make_unique<KdenliveDoc>(undoStack);
        pCore->projectManager()->testSetDocument(document.get());
        REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), false));
        timeline = document->getTimeline(document->uuid());
        REQUIRE(timeline);
        pCore->projectManager()->testSetActiveTimeline(timeline);
        document->loading = false;
        for (int position = 0; position < timeline->getTracksCount(); ++position) {
            const int id = timeline->getTrackIndexFromPosition(position);
            if (timeline->isAudioTrack(id)) {
                audioTrack = id;
            } else {
                videoTracks.push_back(id);
            }
        }
        REQUIRE(videoTracks.size() >= 2);
        REQUIRE(audioTrack >= 0);
        undoStack->clear();
        initialize();
    }

    ~McpProject()
    {
        // Headless ProjectManager does not own the dummy test document.
        if (document && pCore->projectManager()->current() == document.get()) {
            pCore->projectManager()->closeCurrentDocument(false, false);
        }
    }

    void initialize()
    {
        const QJsonObject params{{QStringLiteral("protocolVersion"), QStringLiteral("2025-06-18")},
                                 {QStringLiteral("capabilities"), QJsonObject{}},
                                 {QStringLiteral("clientInfo"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("native-test")}, {QStringLiteral("version"), QStringLiteral("1")}}}};
        const QJsonObject request{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                  {QStringLiteral("id"), ++requestId},
                                  {QStringLiteral("method"), QStringLiteral("initialize")},
                                  {QStringLiteral("params"), params}};
        const QJsonObject reply = server.handleRequest(request);
        INFO(QJsonDocument(reply).toJson(QJsonDocument::Compact).constData());
        REQUIRE_FALSE(reply.contains(QStringLiteral("error")));
        REQUIRE(reply.value(QStringLiteral("result")).isObject());
        const QJsonObject notification{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                       {QStringLiteral("method"), QStringLiteral("notifications/initialized")}};
        CHECK(server.handleRequest(notification).isEmpty());
    }

    QJsonObject call(const QString &name, const QJsonObject &arguments = {})
    {
        INFO("MCP tool " << name.toStdString() << ": " << QJsonDocument(arguments).toJson(QJsonDocument::Compact).constData());
        const QJsonObject reply = server.handleRequest(
            QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                        {QStringLiteral("id"), ++requestId},
                        {QStringLiteral("method"), QStringLiteral("tools/call")},
                        {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("arguments"), arguments}}}});
        INFO(QJsonDocument(reply).toJson(QJsonDocument::Compact).constData());
        REQUIRE_FALSE(reply.contains(QStringLiteral("error")));
        REQUIRE(reply.value(QStringLiteral("result")).isObject());
        return reply.value(QStringLiteral("result")).toObject();
    }

    QJsonObject success(const QString &name, const QJsonObject &arguments = {})
    {
        const QJsonObject result = call(name, arguments);
        INFO(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
        REQUIRE_FALSE(result.value(QStringLiteral("isError")).toBool());
        const QJsonObject content = result.value(QStringLiteral("structuredContent")).toObject();
        REQUIRE(content.value(QStringLiteral("ok")).toBool());
        return content;
    }

    QJsonObject state()
    {
        const QJsonObject result = success(QStringLiteral("get_state"));
        const QJsonObject current = result.value(QStringLiteral("state")).toObject();
        REQUIRE(current.value(QStringLiteral("ready")).toBool());
        REQUIRE_FALSE(current.value(QStringLiteral("expectedState")).toString().isEmpty());
        return current;
    }

    QString token() { return state().value(QStringLiteral("expectedState")).toString(); }

    QJsonObject expected() { return QJsonObject{{QStringLiteral("expectedState"), token()}}; }

    QJsonObject mediaArgs(const QString &binId, int trackId, int start, int sourceIn, int duration, const QString &stream)
    {
        QJsonObject args = expected();
        args.insert(QStringLiteral("binId"), binId);
        args.insert(QStringLiteral("trackId"), trackId);
        args.insert(QStringLiteral("startFrame"), start);
        args.insert(QStringLiteral("sourceInFrame"), sourceIn);
        args.insert(QStringLiteral("durationFrames"), duration);
        args.insert(QStringLiteral("stream"), stream);
        return args;
    }

    QJsonObject binItem(const QString &binId)
    {
        for (const auto &value : state().value(QStringLiteral("bin")).toArray()) {
            const auto item = value.toObject();
            if (item.value(QStringLiteral("id")).toString() == binId) return item;
        }
        FAIL("Imported bin item was not reported by get_state");
        return {};
    }

    QJsonObject awaitImportTerminal(const QString &binId)
    {
        QElapsedTimer deadline;
        deadline.start();
        while (deadline.elapsed() < 10000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            const auto imports = state().value(QStringLiteral("imports")).toArray();
            for (const auto &value : imports) {
                const auto item = value.toObject();
                if (item.value(QStringLiteral("binId")).toString() == binId && item.value(QStringLiteral("status")).toString() != QStringLiteral("loading")) {
                    return item;
                }
            }
            QThread::msleep(1);
        }
        FAIL("Imported media never reached a terminal state");
        return {};
    }
    void awaitMediaReady(const QString &binId)
    {
        const auto clip = pCore->projectItemModel()->getClipByBinID(binId);
        REQUIRE(clip);
        QElapsedTimer deadline;
        deadline.start();
        // Only test code pumps the GUI queue; the MCP import itself stays asynchronous.
        while (!clip->statusReady() && clip->clipStatus() != FileStatus::StatusMissing && deadline.elapsed() < 10000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        INFO("Native imported clip status: " << static_cast<int>(clip->clipStatus()));
        REQUIRE(clip->statusReady());
        const auto item = binItem(binId);
        REQUIRE(item.value(QStringLiteral("ready")).toBool());
        REQUIRE(item.value(QStringLiteral("status")).toString() == QStringLiteral("ready"));
    }

    QJsonObject colorArgs(int trackId, int start, int duration)
    {
        QJsonObject args = expected();
        args.insert(QStringLiteral("trackId"), trackId);
        args.insert(QStringLiteral("startFrame"), start);
        args.insert(QStringLiteral("durationFrames"), duration);
        args.insert(QStringLiteral("color"), QStringLiteral("#ff0000ff"));
        args.insert(QStringLiteral("name"), QStringLiteral("MCP native test"));
        return args;
    }

    void rejectWithoutChange(const QString &name, const QJsonObject &arguments)
    {
        const QString beforeToken = token();
        const int beforeUndoIndex = undoStack->index();
        const int beforeUndoCount = undoStack->count();
        const int beforeClips = timeline->getClipsCount();
        const auto beforeBin = pCore->projectItemModel()->getAllClipIds();
        const QJsonObject result = call(name, arguments);
        INFO(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
        REQUIRE(result.value(QStringLiteral("isError")).toBool());
        const QJsonObject content = result.value(QStringLiteral("structuredContent")).toObject();
        REQUIRE_FALSE(content.value(QStringLiteral("ok")).toBool(true));
        REQUIRE(content.value(QStringLiteral("error")).isObject());
        CHECK(token() == beforeToken);
        CHECK(undoStack->index() == beforeUndoIndex);
        CHECK(undoStack->count() == beforeUndoCount);
        CHECK(timeline->getClipsCount() == beforeClips);
        CHECK(pCore->projectItemModel()->getAllClipIds() == beforeBin);
        const auto guides = document->getGuideModel(timeline->uuid());
        REQUIRE(timeline->checkConsistency(guides ? guides->getSnapPoints() : std::vector<int>{}));
    }

    std::shared_ptr<DocUndoStack> undoStack;
    std::unique_ptr<KdenliveDoc> document;
    std::shared_ptr<TimelineItemModel> timeline;
    std::vector<int> videoTracks;
    int audioTrack{-1};
    KdenliveMcpServer server;

private:
    int requestId{0};
};

QString pngFixture(const QTemporaryDir &directory)
{
    const QString path = directory.filePath(QStringLiteral("fixture.png"));
    QImage image(32, 32, QImage::Format_RGB32);
    image.fill(Qt::red);
    REQUIRE(image.save(path, "PNG"));
    return path;
}

QString wavFixture(const QTemporaryDir &directory)
{
    const QString path = directory.filePath(QStringLiteral("fixture.wav"));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    // One second of stereo 48 kHz signed 16-bit PCM, without external encoders.
    QByteArray samples;
    QDataStream audio(&samples, QIODevice::WriteOnly);
    audio.setByteOrder(QDataStream::LittleEndian);
    for (int sample = 0; sample < 48000; ++sample) {
        const double phase = 6.283185307179586 * sample / 48000.0;
        audio << qint16(8192 * std::sin(440 * phase)) << qint16(4096 * std::sin(660 * phase));
    }
    REQUIRE(audio.status() == QDataStream::Ok);
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    REQUIRE(file.write("RIFF", 4) == 4);
    stream << quint32(36 + samples.size());
    REQUIRE(file.write("WAVEfmt ", 8) == 8);
    stream << quint32(16) << quint16(1) << quint16(2) << quint32(48000) << quint32(192000) << quint16(4) << quint16(16);
    REQUIRE(file.write("data", 4) == 4);
    stream << quint32(samples.size());
    REQUIRE(file.write(samples) == samples.size());
    REQUIRE(stream.status() == QDataStream::Ok);
    file.close();
    return path;
}
QString corruptMp4Fixture(const QTemporaryDir &directory)
{
    const QString path = directory.filePath(QStringLiteral("broken.mp4"));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::BigEndian);
    // A recognizable MP4 file-type box, with no moov/media data for the decoder.
    stream << quint32(24);
    REQUIRE(file.write("ftypmp42", 8) == 8);
    stream << quint32(0);
    REQUIRE(file.write("isommp42", 8) == 8);
    REQUIRE(stream.status() == QDataStream::Ok);
    file.close();
    REQUIRE(QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchContent).name().startsWith(QStringLiteral("video/")));
    return path;
}

QString avFixture(const QTemporaryDir &directory)
{
    const QString path = directory.filePath(QStringLiteral("fixture.mp4"));
    // One second of 25 fps red 32x32 H.264 and mono 8 kHz AAC sine audio.
    // Generated from FFmpeg lavfi sources; embedding it keeps the native test self-contained.
    const QByteArray encoded("AAAAIGZ0eXBpc29tAAACAGlzb21pc28yYXZjMW1wNDEAAAZTbW9vdgAAAGxtdmhkAAAAAAAAAAAAAAAAAAAfQAAAH0AAAQAAAQAAAAAAAAAAAAAAAAEAAAAA"
                             "AAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAwAAAwF0cmFrAAAAXHRraGQAAAADAAAAAAAAAAAAAAAB"
                             "AAAAAAAAH0AAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAACAAAAAgAAAAAAAkZWR0cwAAABxlbHN0AAAAAAAA"
                             "AAEAAB9AAAAAAAABAAAAAAJ5bWRpYQAAACBtZGhkAAAAAAAAAAAAAAAAAAAyAAAAMgBVxAAAAAAALWhkbHIAAAAAAAAAAHZpZGUAAAAAAAAAAAAAAABWaWRl"
                             "b0hhbmRsZXIAAAACJG1pbmYAAAAUdm1oZAAAAAEAAAAAAAAAAAAAACRkaW5mAAAAHGRyZWYAAAAAAAAAAQAAAAx1cmwgAAAAAQAAAeRzdGJsAAAAuHN0c2QA"
                             "AAAAAAAAAQAAAKhhdmMxAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAACAAIABIAAAASAAAAAAAAAABFExhdmM2My4xLjEwMSBsaWJ4MjY0AAAAAAAAAAAAAAAA"
                             "GP//AAAALmF2Y0MBQsAK/+EAFmdCwAraJbARAAADAAEAAAMAMo8SJqABAAVozgJcgAAAABBwYXNwAAAAAQAAAAEAAAAUYnRydAAAAAAAABowAAAAAAAAABhz"
                             "dHRzAAAAAAAAAAEAAAAZAAACAAAAABRzdHNzAAAAAAAAAAEAAAABAAAATHN0c2MAAAAAAAAABQAAAAEAAAABAAAAAQAAAAIAAAADAAAAAQAAAAYAAAAEAAAA"
                             "AQAAAAcAAAADAAAAAQAAAAkAAAACAAAAAQAAAHhzdHN6AAAAAAAAAAAAAAAZAAACbgAAAAkAAAAJAAAACQAAAAkAAAAJAAAACQAAAAkAAAAJAAAACQAAAAkA"
                             "AAAJAAAACQAAAAkAAAAJAAAACQAAAAkAAAAJAAAACQAAAAkAAAAJAAAACQAAAAkAAAAJAAAACQAAADRzdGNvAAAAAAAAAAkAAAd7AAAKuQAAC2AAAAv6AAAM"
                             "jQAADSsAAA3PAAAOmAAAD10AAAJ9dHJhawAAAFx0a2hkAAAAAwAAAAAAAAAAAAAAAgAAAAAAAB9AAAAAAAAAAAAAAAABAQAAAAABAAAAAAAAAAAAAAAAAAAA"
                             "AQAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAJGVkdHMAAAAcZWxzdAAAAAAAAAABAAAfQAAABAAAAQAAAAAB9W1kaWEAAAAgbWRoZAAAAAAAAAAAAAAA"
                             "AAAAH0AAACNAVcQAAAAAAC1oZGxyAAAAAAAAAABzb3VuAAAAAAAAAAAAAAAAU291bmRIYW5kbGVyAAAAAaBtaW5mAAAAEHNtaGQAAAAAAAAAAAAAACRkaW5m"
                             "AAAAHGRyZWYAAAAAAAAAAQAAAAx1cmwgAAAAAQAAAWRzdGJsAAAAfnN0c2QAAAAAAAAAAQAAAG5tcDRhAAAAAAAAAAEAAAAAAAAAAAABABAAAAAAH0AAAAAA"
                             "ADZlc2RzAAAAAAOAgIAlAAIABICAgBdAFQAAAAAAKA8AACgPBYCAgAUViFblAAaAgIABAgAAABRidHJ0AAAAAAAAKA8AACgPAAAAIHN0dHMAAAAAAAAAAgAA"
                             "AAgAAAQAAAAAAQAAA0AAAAAcc3RzYwAAAAAAAAABAAAAAQAAAAEAAAABAAAAOHN0c3oAAAAAAAAAAAAAAAkAAAD4AAAA0AAAAIwAAAB/AAAAeAAAAIMAAACA"
                             "AAAArgAAAKoAAAA0c3RjbwAAAAAAAAAJAAAGgwAACekAAArUAAALewAADBUAAAyoAAANTwAADeoAAA6zAAAAGnNncGQBAAAAcm9sbAAAAAIAAAAB//8AAAAc"
                             "c2JncAAAAAByb2xsAAAAAQAAAAkAAAABAAAAYXVkdGEAAABZbWV0YQAAAAAAAAAhaGRscgAAAAAAAAAAbWRpcmFwcGwAAAAAAAAAAAAAAAAsaWxzdAAAACSp"
                             "dG9vAAAAHGRhdGEAAAABAAAAAExhdmY2My4xLjEwMQAAAAhmcmVlAAAI9G1kYXTcAExhdmM2My4xLjEwMQACKKVZkSwqEUkulf2opWZfOed3q0kkSRJBbVs2"
                             "1bNtWzmLMOYsw5izD2V2T2V2T2V3TsbYuxti7O2Ls7YvF3FvF3FujtG6O0bo7RoSEhISEhISEhISEhISEhISEhKUJCZpjiWaaaZnZ2kaRHR6K+OC6R/57UpK"
                             "BcmZZkoEoEKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK"
                             "KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKLgAAAlIGBf//TtxF6b3m2Ui3lizYINkj7u94MjY0IC0gY29yZSAxNjUgcjMyMjMgMDQ4MGNiMCAtIEguMjY0L01Q"
                             "RUctNCBBVkMgY29kZWMgLSBDb3B5bGVmdCAyMDAzLTIwMjUgLSBodHRwOi8vd3d3LnZpZGVvbGFuLm9yZy94MjY0Lmh0bWwgLSBvcHRpb25zOiBjYWJhYz0w"
                             "IHJlZj0xIGRlYmxvY2s9MDowOjAgYW5hbHlzZT0wOjAgbWU9ZGlhIHN1Ym1lPTAgcHN5PTEgcHN5X3JkPTEuMDA6MC4wMCBtaXhlZF9yZWY9MCBtZV9yYW5n"
                             "ZT0xNiBjaHJvbWFfbWU9MSB0cmVsbGlzPTAgOHg4ZGN0PTAgY3FtPTAgZGVhZHpvbmU9MjEsMTEgZmFzdF9wc2tpcD0xIGNocm9tYV9xcF9vZmZzZXQ9MCB0"
                             "aHJlYWRzPTEgbG9va2FoZWFkX3RocmVhZHM9MSBzbGljZWRfdGhyZWFkcz0wIG5yPTAgZGVjaW1hdGU9MSBpbnRlcmxhY2VkPTAgYmx1cmF5X2NvbXBhdD0w"
                             "IGNvbnN0cmFpbmVkX2ludHJhPTAgYmZyYW1lcz0wIHdlaWdodHA9MCBrZXlpbnQ9MjUga2V5aW50X21pbj0yIHNjZW5lY3V0PTAgaW50cmFfcmVmcmVzaD0w"
                             "IHJjPWNyZiBtYnRyZWU9MCBjcmY9MzUuMCBxY29tcD0wLjYwIHFwbWluPTAgcXBtYXg9NjkgcXBzdGVwPTQgaXBfcmF0aW89MS40MCBhcT0wAIAAAAAUZYiE"
                             "OhGKAAIDMcAA3HAAEDCTrrwBAJzYvxRL0TBKjzRL5vec+v/21/+ftrXGtf1zffr/6V/+vt1rU1+m879f/Tf/6/HHF6+P9c336/+n6/+XtxxroJeeeeSkw0h5"
                             "jzHmMeY8x4JDJ6y4rE4rEvfT6PfT6Pee555aUrSkAECDw5xQHmc7KDPPc72uh55P052a6KHnnmc97A2YGnrEw2WFVilPIwULnoonoonoXOvA6lIIINjYu2bN"
                             "kd7Zs2R3tmy2zZHfvR2Ix2G+hs2ADbQ47xU8885zwicTqnnnnPOqdU6p55+AAAAABUGaICaLAAAABUGaQCqLAAAABUGaYCqLAQjyi8JyYpVZVZvfP9/6f/b2"
                             "1xrVy5cvv2zmvIGfQDv5g77wg/w2HfeEZPgwz+5B/4Az+5B/4MHfchk+Gxn99AA+A2Bk+Mwd9/cD4zAe/uAj4MM+gh8fAT7+5DJ8GGf3A+Pgwz+5DJ8Ae/uQ"
                             "yNmHv7kMnwHm/hB5P4xEAP4QftjMv8A2gCgAABUAZQcAAAAFQZqAKosAAAAFQZqgKosAAAAFQZrAKosBPjKAJPkNhYJnAIiAJnf/8G/z96/T75+n1WZz//d/"
                             "/9Tz4/n9ufr5yZbn22AB9Pp9Pp9Po888oB555554LDzzzzzwB2JeS88888888889V1dN/zRrOdTN8AAOfK+0yMtXVNXVoBb9+gCcQBjGQAYiIAiALACYAkAG"
                             "IAexAFwcAAAABUGa4CqLAAAABUGbACqLAAAABUGbICqLAOIykgKgCIgCJ3z9H2+n7fh+2vOb/1//5f/9vx5+fv9ta99656zd8AGBgZ7WgYGBgYGBgYGBsncG"
                             "BgYGBgZ8HBgYGBgYGd8VO4MDAwMDAwMOBgYGBhJmP5j+fnaf5/z1AUgASiAICwAkRAEABkAFABQAIACYAsDgAAAABUGbQCqLAAAABUGbYCqLAAAABUGbgCqL"
                             "AT4ygCT7EwRGwRGAREARO//31X6fVfb4r861mT/+9/+r2+f/f/0+M+avelXYAH0+n010UUUUawKKKKKKAwUUUUUUAOmAFFFC6KKKKKKCyiyiyjkhg2qKX3oU"
                             "AB6+FFSCg7ImzZsAl3yTSTSM7OEpgCIsAKEQBEAXAEgBSAAiAOAAkDgAAAAFQZugKosAAAAFQZvAKosAAAAFQZvgKosAAAAFQZoAKosBPjKAIhARMsTBE4BF"
                             "T//v+f3r+v71/X8N5X/9L//1PPv9/tOvtn/z/6c6vnVgAJUA888888EB555554A7A88888889pee/f7EHEHEBT6ENS4KaYwAA8e/O5YWPy1n5a5gUwgEoACA"
                             "sAJNmzZs2bNn7IgCAAuAMQAQAEwBYAVBwAAAAAVBmiAqiwAAAAVBmkAqiwAAAAVBmmAqiwEeMovmAUVSqz3//s//u61cuSSRIQMVqxrQyvuWrHpQyvuWrHpQ"
                             "yvuWrHpqZXItWOFTK5L48cKtNcl/UANIYB1dwGnpqZX3Xx49NV1aL48V1aa5L48cKtNaL+rFdWmtF/ViurT0ov6sV1aelDOruXVp6UM6u5atPShlfdfUnpQz"
                             "q7lq09NTOruvVj01Mr7r1Y9NTK5ArHBDK5FqxwqZXJerHCoVyXqxXUysXqxXVwAAAAVBmoAqiwAAAAVBmqAqiwAAAAVBmsAqiwE4MovmUBRVVWevH41q5ckk"
                             "kQgK58q2TQIVBMtk25CoMq2TbnFQPrZNucVBlaZM6hUD7TJtzioMoB3cjpByGBDTcy3IYENc8y3IYENNzOuc0CGueZbkMCGueZbnNAhXPlW5zQIVz5Vuc25D"
                             "XPlWzm3Ia58rTObchXPlWybchUGVpnNucVBlaZNucVA+0yZ1CoH1smWhUCGmTOuKgfaZMtxUCGmTLcVAhTJuAAAABUGa4CqLAAAABUGbACqL");
    const auto data = QByteArray::fromBase64(encoded);
    REQUIRE(data.size() == 3951);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(data) == data.size());
    file.close();
    return path;
}
QString importFixture(McpProject &project, const QString &path)
{
    auto arguments = project.expected();
    arguments.insert(QStringLiteral("path"), path);
    const auto imported = project.success(QStringLiteral("import_media"), arguments);
    const QString binId = imported.value(QStringLiteral("binId")).toString();
    REQUIRE_FALSE(binId.isEmpty());
    project.awaitMediaReady(binId);
    return binId;
}
QVector<QPair<double, double>> renderedAudioRms(const std::shared_ptr<TimelineItemModel> &timeline, int start, int duration)
{
    QVector<QPair<double, double>> result;
    for (int position = start; position < start + duration; ++position) {
        timeline->tractor()->seek(position);
        std::unique_ptr<Mlt::Frame> frame(timeline->tractor()->get_frame());
        REQUIRE(frame);
        mlt_audio_format format = mlt_audio_f32le;
        int frequency = 48000, channels = 2, samples = 1920;
        const auto *buffer = static_cast<float *>(frame->get_audio(format, frequency, channels, samples));
        REQUIRE(buffer);
        REQUIRE(format == mlt_audio_f32le);
        REQUIRE(frequency == 48000);
        REQUIRE(channels == 2);
        REQUIRE(samples > 0);
        double left = 0, right = 0;
        for (int sample = 0; sample < samples; ++sample) {
            left += double(buffer[sample * 2]) * buffer[sample * 2];
            right += double(buffer[sample * 2 + 1]) * buffer[sample * 2 + 1];
        }
        result.append({std::sqrt(left / samples), std::sqrt(right / samples)});
    }
    return result;
}
QJsonObject transformFrame(int frame, double x, double y, double width, double height, double opacity, double rotation)
{
    return {{QStringLiteral("frame"), frame},      {QStringLiteral("x"), x},           {QStringLiteral("y"), y},
            {QStringLiteral("width"), width},      {QStringLiteral("height"), height}, {QStringLiteral("opacity"), opacity},
            {QStringLiteral("rotation"), rotation}};
}
QJsonObject transformArgs(McpProject &project, int clipId, const QJsonArray &frames)
{
    auto arguments = project.expected();
    arguments.insert(QStringLiteral("clipId"), clipId);
    arguments.insert(QStringLiteral("keyframes"), frames);
    return arguments;
}
QJsonObject audioArgs(McpProject &project, int clipId, double gain, int fadeIn, int fadeOut)
{
    auto arguments = project.expected();
    arguments.insert(QStringLiteral("clipId"), clipId);
    arguments.insert(QStringLiteral("gainDb"), gain);
    arguments.insert(QStringLiteral("fadeInFrames"), fadeIn);
    arguments.insert(QStringLiteral("fadeOutFrames"), fadeOut);
    return arguments;
}
QJsonObject appliedEffect(const QJsonObject &result, const QString &kind)
{
    REQUIRE(result.value(QStringLiteral("appliedEffects")).isArray());
    for (const auto &value : result.value(QStringLiteral("appliedEffects")).toArray()) {
        const auto effect = value.toObject();
        if (effect.value(QStringLiteral("kind")).toString() == kind) return effect;
    }
    FAIL("Expected native effect was absent from tool readback");
    return {};
}
std::shared_ptr<EffectItemModel> effectInstance(const std::shared_ptr<EffectStackModel> &stack, int instanceId)
{
    for (int row = 0; row < stack->rowCount(); ++row) {
        const auto effect = std::dynamic_pointer_cast<EffectItemModel>(stack->getEffectStackRow(row));
        if (effect && effect->getId() == instanceId) return effect;
    }
    return nullptr;
}
void requireTransformSample(const std::shared_ptr<EffectItemModel> &effect, int sourceFrame, double x, double y, double width, double height, double opacity,
                            double rotation)
{
    REQUIRE(effect);
    const auto rect = effect->filter().anim_get_rect("rect", sourceFrame);
    CHECK(rect.x == Approx(x).margin(0.0001));
    CHECK(rect.y == Approx(y).margin(0.0001));
    CHECK(rect.w == Approx(width).margin(0.0001));
    CHECK(rect.h == Approx(height).margin(0.0001));
    CHECK(rect.o == Approx(opacity).margin(0.0001));
    CHECK(effect->filter().anim_get_double("rotation", sourceFrame) == Approx(rotation).margin(0.0001));
}

} // namespace

TEST_CASE("MCP edits use native clip operations and one undo step per edit", "[MCP]")
{
    McpProject project;
    project.open();
    const int video1 = project.videoTracks.at(0);
    const int video2 = project.videoTracks.at(1);
    const auto initialBinCount = pCore->projectItemModel()->getAllClipIds().size();

    const QJsonObject added = project.success(QStringLiteral("add_color_clip"), project.colorArgs(video1, 10, 40));
    REQUIRE(added.value(QStringLiteral("clipId")).isDouble());
    const int clipId = added.value(QStringLiteral("clipId")).toInt(-1);
    const QString binId = added.value(QStringLiteral("binId")).toString();
    REQUIRE(clipId >= 0);
    REQUIRE_FALSE(binId.isEmpty());
    REQUIRE(project.timeline->isClip(clipId));
    CHECK(project.timeline->getClipPosition(clipId) == 10);
    CHECK(project.timeline->getClipPlaytime(clipId) == 40);
    CHECK(project.timeline->getClipTrackId(clipId) == video1);
    CHECK(project.timeline->getClipInOut(clipId) == std::make_pair(0, 39));
    CHECK(project.undoStack->index() == 1);
    REQUIRE(project.timeline->checkConsistency());

    QJsonObject move = project.expected();
    move.insert(QStringLiteral("clipId"), clipId);
    move.insert(QStringLiteral("trackId"), video2);
    move.insert(QStringLiteral("startFrame"), 50);
    project.success(QStringLiteral("move_clip"), move);
    CHECK(project.timeline->getClipPosition(clipId) == 50);
    CHECK(project.timeline->getClipTrackId(clipId) == video2);
    CHECK(project.undoStack->index() == 2);

    QJsonObject cut = project.expected();
    cut.insert(QStringLiteral("clipId"), clipId);
    cut.insert(QStringLiteral("frame"), 65);
    const QJsonObject cutResult = project.success(QStringLiteral("cut_clip"), cut);
    const int left = cutResult.value(QStringLiteral("leftClipId")).toInt(-1);
    const int right = cutResult.value(QStringLiteral("rightClipId")).toInt(-1);
    REQUIRE(project.timeline->isClip(left));
    REQUIRE(project.timeline->isClip(right));
    CHECK(left != right);
    CHECK(project.timeline->getClipPosition(left) == 50);
    CHECK(project.timeline->getClipPlaytime(left) == 15);
    CHECK(project.timeline->getClipPosition(right) == 65);
    CHECK(project.timeline->getClipPlaytime(right) == 25);
    CHECK(project.undoStack->index() == 3);
    REQUIRE(project.timeline->checkConsistency());

    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.timeline->getClipsCount() == 1);
    CHECK(project.timeline->getClipPlaytime(clipId) == 40);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.timeline->getClipPosition(clipId) == 10);
    CHECK(project.timeline->getClipTrackId(clipId) == video1);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.timeline->getClipsCount() == 0);
    CHECK(pCore->projectItemModel()->getAllClipIds().size() == initialBinCount);
    CHECK(project.undoStack->index() == 0);
    REQUIRE(project.timeline->checkConsistency());

    for (int i = 0; i < 3; ++i) {
        project.success(QStringLiteral("redo"), project.expected());
        REQUIRE(project.timeline->checkConsistency());
    }
    CHECK(project.timeline->getClipsCount() == 2);
    CHECK(project.timeline->getClipPosition(left) == 50);
    CHECK(project.timeline->getClipPlaytime(left) == 15);
    CHECK(project.timeline->getClipPosition(right) == 65);
    CHECK(project.timeline->getClipPlaytime(right) == 25);
}

TEST_CASE("MCP state tokens reject manual edits and undo-history branches", "[MCP]")
{
    McpProject project;
    project.open();
    const int track = project.videoTracks.at(0);
    const int clipId = project.success(QStringLiteral("add_color_clip"), project.colorArgs(track, 0, 20)).value(QStringLiteral("clipId")).toInt(-1);
    REQUIRE(project.timeline->isClip(clipId));
    const QString beforeManual = project.token();
    const int originalIndex = project.undoStack->index();

    REQUIRE(project.timeline->requestClipMove(clipId, track, 40, true, true, true));
    CHECK(project.token() != beforeManual);
    project.rejectWithoutChange(QStringLiteral("undo"), QJsonObject{{QStringLiteral("expectedState"), beforeManual}});

    project.undoStack->undo();
    REQUIRE(project.timeline->getClipPosition(clipId) == 0);
    REQUIRE(project.undoStack->index() == originalIndex);
    // The image and undo index match the earlier state, but its token must still be stale.
    CHECK(project.token() != beforeManual);
    project.rejectWithoutChange(QStringLiteral("undo"), QJsonObject{{QStringLiteral("expectedState"), beforeManual}});

    REQUIRE(project.timeline->requestClipMove(clipId, track, 80, true, true, true));
    const QString priorBranch = project.token();
    const int branchIndex = project.undoStack->index();
    project.undoStack->undo();
    REQUIRE(project.timeline->requestClipMove(clipId, track, 100, true, true, true));
    REQUIRE(project.undoStack->index() == branchIndex);
    CHECK(project.token() != priorBranch);
    project.rejectWithoutChange(QStringLiteral("undo"), QJsonObject{{QStringLiteral("expectedState"), priorBranch}});
    REQUIRE(project.timeline->getClipPosition(clipId) == 100);
}

TEST_CASE("MCP rejects invalid and conflicting edits without changing native state", "[MCP]")
{
    McpProject project;
    project.open();
    const int track = project.videoTracks.at(0);

    SECTION("Malformed numeric fields, color, and unknown IDs")
    {
        QJsonObject args = project.colorArgs(track, 0, 20);
        args.insert(QStringLiteral("startFrame"), 0.5);
        project.rejectWithoutChange(QStringLiteral("add_color_clip"), args);
        args = project.colorArgs(track, 0, 20);
        args.insert(QStringLiteral("durationFrames"), 0);
        project.rejectWithoutChange(QStringLiteral("add_color_clip"), args);
        args = project.colorArgs(track, 0, 20);
        args.insert(QStringLiteral("startFrame"), static_cast<double>(std::numeric_limits<int>::max()) + 1.0);
        project.rejectWithoutChange(QStringLiteral("add_color_clip"), args);
        args = project.colorArgs(track, 0, 20);
        args.insert(QStringLiteral("color"), QStringLiteral("#gg0000ff"));
        project.rejectWithoutChange(QStringLiteral("add_color_clip"), args);
        args = project.colorArgs(std::numeric_limits<int>::max(), 0, 20);
        project.rejectWithoutChange(QStringLiteral("add_color_clip"), args);
    }

    SECTION("Color clips cannot be placed on audio or locked tracks")
    {
        project.rejectWithoutChange(QStringLiteral("add_color_clip"), project.colorArgs(project.audioTrack, 0, 20));
        project.timeline->setTrackProperty(track, QStringLiteral("kdenlive:locked_track"), QStringLiteral("1"));
        project.rejectWithoutChange(QStringLiteral("add_color_clip"), project.colorArgs(track, 0, 20));
    }

    SECTION("Overlap, missing clips, and cuts at clip boundaries fail")
    {
        const int clipId = project.success(QStringLiteral("add_color_clip"), project.colorArgs(track, 10, 30)).value(QStringLiteral("clipId")).toInt(-1);
        REQUIRE(project.timeline->isClip(clipId));
        project.rejectWithoutChange(QStringLiteral("add_color_clip"), project.colorArgs(track, 20, 10));
        const int second = project.success(QStringLiteral("add_color_clip"), project.colorArgs(track, 60, 20)).value(QStringLiteral("clipId")).toInt(-1);
        REQUIRE(project.timeline->isClip(second));
        QJsonObject move = project.expected();
        move.insert(QStringLiteral("clipId"), second);
        move.insert(QStringLiteral("trackId"), track);
        move.insert(QStringLiteral("startFrame"), 20);
        project.rejectWithoutChange(QStringLiteral("move_clip"), move);
        CHECK(project.timeline->getClipPosition(second) == 60);
        move = project.expected();
        move.insert(QStringLiteral("clipId"), std::numeric_limits<int>::max());
        move.insert(QStringLiteral("trackId"), track);
        move.insert(QStringLiteral("startFrame"), 100);
        project.rejectWithoutChange(QStringLiteral("move_clip"), move);

        for (int frame : {10, 40}) {
            QJsonObject cut = project.expected();
            cut.insert(QStringLiteral("clipId"), clipId);
            cut.insert(QStringLiteral("frame"), frame);
            project.rejectWithoutChange(QStringLiteral("cut_clip"), cut);
        }
        QJsonObject cut = project.expected();
        cut.insert(QStringLiteral("clipId"), clipId);
        cut.insert(QStringLiteral("frame"), 15.25);
        project.rejectWithoutChange(QStringLiteral("cut_clip"), cut);
        CHECK(project.timeline->getClipPosition(clipId) == 10);
        CHECK(project.timeline->getClipPlaytime(clipId) == 30);
    }
}

TEST_CASE("MCP automated save failures return errors without opening dialogs", "[MCP]")
{
    McpProject project;
    project.open();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QString message;
    CHECK_FALSE(project.document->saveSceneList(directory.filePath(QStringLiteral("corrupt.kdenlive")), QStringLiteral("invalid xml"), false, &message));
    CHECK_FALSE(message.isEmpty());
    const QString scene = QStringLiteral("<mlt><playlist id=\"main_bin\"/><tractor><track producer=\"main_bin\"/></tractor></mlt>");
    CHECK_FALSE(project.document->saveSceneList(directory.filePath(QStringLiteral("missing/output.kdenlive")), scene, false, &message));
    CHECK_FALSE(message.isEmpty());
    QFile existing(directory.filePath(QStringLiteral("existing.kdenlive")));
    REQUIRE(existing.open(QIODevice::WriteOnly));
    REQUIRE(existing.write("preserve", 8) == 8);
    existing.close();
    CHECK_FALSE(project.document->saveSceneList(existing.fileName(), scene, false, &message));
    CHECK_FALSE(message.isEmpty());
    REQUIRE(existing.open(QIODevice::ReadOnly));
    CHECK(existing.readAll() == QByteArray("preserve"));
}

TEST_CASE("MCP rejects unbounded request IDs before dispatch", "[MCP]")
{
    McpProject project;
    project.open();
    const auto before = project.state();
    QJsonObject request{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                        {QStringLiteral("id"), QString(129, QLatin1Char('x'))},
                        {QStringLiteral("method"), QStringLiteral("tools/list")}};
    const auto response = project.server.handleRequest(request);
    CHECK(response.value(QStringLiteral("id")).isNull());
    REQUIRE(response.value(QStringLiteral("error")).isObject());
    CHECK(project.state() == before);
}

TEST_CASE("MCP imports PNG media and inserts exact frame ranges", "[MCP][MCPMedia]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = pngFixture(directory);
    McpProject project;
    project.open();
    const int track = project.videoTracks.at(0);
    const QString beforeImport = project.token();
    const auto initialBinCount = pCore->projectItemModel()->getAllClipIds().size();
    QJsonObject arguments = project.expected();
    arguments.insert(QStringLiteral("path"), path);
    const auto imported = project.success(QStringLiteral("import_media"), arguments);
    const QString binId = imported.value(QStringLiteral("binId")).toString();
    REQUIRE_FALSE(binId.isEmpty());
    REQUIRE(pCore->projectItemModel()->getClipByBinID(binId));
    CHECK(project.undoStack->index() == 1);
    CHECK(project.timeline->getClipsCount() == 0);
    if (imported.value(QStringLiteral("importStatus")).toString() == QStringLiteral("loading")) {
        project.rejectWithoutChange(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, track, 12, 0, 8, QStringLiteral("video")));
    }
    project.awaitMediaReady(binId);
    const auto source = project.binItem(binId);
    CHECK(source.value(QStringLiteral("mediaType")).toString() == QStringLiteral("image"));
    CHECK(source.value(QStringLiteral("hasVideo")).toBool());
    CHECK_FALSE(source.value(QStringLiteral("hasAudio")).toBool());
    CHECK(project.state().value(QStringLiteral("freshProjectEligible")).toBool());
    CHECK(pCore->projectItemModel()->getAllClipIds().size() == initialBinCount + 1);

    auto stale = project.mediaArgs(binId, track, 12, 0, 8, QStringLiteral("video"));
    stale.insert(QStringLiteral("expectedState"), beforeImport);
    project.rejectWithoutChange(QStringLiteral("insert_media_clip"), stale);
    const auto inserted = project.success(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, track, 12, 0, 8, QStringLiteral("video")));
    const int clipId = inserted.value(QStringLiteral("clipId")).toInt(-1);
    REQUIRE(project.timeline->isClip(clipId));
    CHECK(project.timeline->getClipBinId(clipId) == binId);
    CHECK(project.timeline->getClipTrackId(clipId) == track);
    CHECK(project.timeline->getClipPosition(clipId) == 12);
    CHECK(project.timeline->getClipPlaytime(clipId) == 8);
    CHECK(project.timeline->getClipInOut(clipId) == std::make_pair(0, 7));
    CHECK(project.undoStack->index() == 2);
    REQUIRE(project.timeline->checkConsistency());

    project.rejectWithoutChange(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, track, 13, 0, 2, QStringLiteral("video")));
    project.rejectWithoutChange(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, project.audioTrack, 0, 0, 8, QStringLiteral("video")));
    project.rejectWithoutChange(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, project.audioTrack, 0, 0, 8, QStringLiteral("audio")));
    auto invalid = project.mediaArgs(binId, track, 30, 0, 8, QStringLiteral("video"));
    invalid.insert(QStringLiteral("sourceInFrame"), -1);
    project.rejectWithoutChange(QStringLiteral("insert_media_clip"), invalid);
    invalid = project.mediaArgs(binId, track, 30, 0, 0, QStringLiteral("video"));
    project.rejectWithoutChange(QStringLiteral("insert_media_clip"), invalid);
    const int otherTrack = project.videoTracks.at(1);
    project.timeline->setTrackProperty(otherTrack, QStringLiteral("kdenlive:locked_track"), QStringLiteral("1"));
    project.rejectWithoutChange(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, otherTrack, 0, 0, 8, QStringLiteral("video")));
    project.timeline->setTrackProperty(otherTrack, QStringLiteral("kdenlive:locked_track"), QStringLiteral("0"));

    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.timeline->getClipsCount() == 0);
    REQUIRE(pCore->projectItemModel()->getClipByBinID(binId));
    CHECK(project.undoStack->index() == 1);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK_FALSE(pCore->projectItemModel()->getClipByBinID(binId));
    CHECK(pCore->projectItemModel()->getAllClipIds().size() == initialBinCount);
    CHECK(project.undoStack->index() == 0);
    project.success(QStringLiteral("redo"), project.expected());
    project.awaitMediaReady(binId);
    project.success(QStringLiteral("redo"), project.expected());
    REQUIRE(project.timeline->isClip(clipId));
    CHECK(project.timeline->getClipPosition(clipId) == 12);
    CHECK(project.timeline->getClipPlaytime(clipId) == 8);
    CHECK(project.timeline->getClipInOut(clipId) == std::make_pair(0, 7));
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("MCP imports real audio and preserves inclusive source trims", "[MCP][MCPMedia]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = wavFixture(directory);
    McpProject project;
    project.open();
    QJsonObject arguments = project.expected();
    arguments.insert(QStringLiteral("path"), path);
    const auto imported = project.success(QStringLiteral("import_media"), arguments);
    const QString binId = imported.value(QStringLiteral("binId")).toString();
    REQUIRE_FALSE(binId.isEmpty());
    project.awaitMediaReady(binId);
    const auto source = project.binItem(binId);
    CHECK(source.value(QStringLiteral("mediaType")).toString() == QStringLiteral("audio"));
    CHECK(source.value(QStringLiteral("hasAudio")).toBool());
    CHECK_FALSE(source.value(QStringLiteral("hasVideo")).toBool());
    const int sourceDuration = source.value(QStringLiteral("durationFrames")).toInt();
    REQUIRE(sourceDuration >= 20);
    CHECK(project.undoStack->index() == 1);

    project.rejectWithoutChange(QStringLiteral("insert_media_clip"),
                                project.mediaArgs(binId, project.audioTrack, 4, sourceDuration, 1, QStringLiteral("audio")));
    project.rejectWithoutChange(QStringLiteral("insert_media_clip"),
                                project.mediaArgs(binId, project.audioTrack, 4, sourceDuration - 1, 2, QStringLiteral("audio")));
    project.rejectWithoutChange(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, project.videoTracks.at(0), 4, 3, 7, QStringLiteral("audio")));
    project.rejectWithoutChange(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, project.videoTracks.at(0), 4, 3, 7, QStringLiteral("video")));
    project.timeline->setTrackProperty(project.audioTrack, QStringLiteral("kdenlive:locked_track"), QStringLiteral("1"));
    project.rejectWithoutChange(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, project.audioTrack, 4, 3, 7, QStringLiteral("audio")));
    project.timeline->setTrackProperty(project.audioTrack, QStringLiteral("kdenlive:locked_track"), QStringLiteral("0"));

    const auto inserted = project.success(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, project.audioTrack, 4, 3, 7, QStringLiteral("audio")));
    const int clipId = inserted.value(QStringLiteral("clipId")).toInt(-1);
    REQUIRE(project.timeline->isClip(clipId));
    CHECK(project.timeline->getClipState(clipId).first == PlaylistState::AudioOnly);
    CHECK(project.timeline->getClipTrackId(clipId) == project.audioTrack);
    CHECK(project.timeline->getClipPosition(clipId) == 4);
    CHECK(project.timeline->getClipPlaytime(clipId) == 7);
    CHECK(project.timeline->getClipInOut(clipId) == std::make_pair(3, 9));
    CHECK(project.undoStack->index() == 2);
    REQUIRE(project.timeline->checkConsistency());
    project.rejectWithoutChange(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, project.audioTrack, 5, 0, 2, QStringLiteral("audio")));
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.timeline->getClipsCount() == 0);
    CHECK(project.undoStack->index() == 1);
    project.success(QStringLiteral("redo"), project.expected());
    REQUIRE(project.timeline->isClip(clipId));
    CHECK(project.timeline->getClipPosition(clipId) == 4);
    CHECK(project.timeline->getClipPlaytime(clipId) == 7);
    CHECK(project.timeline->getClipInOut(clipId) == std::make_pair(3, 9));
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("MCP refuses missing and unsupported media without changing the project", "[MCP][MCPMedia]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QFile unsupported(directory.filePath(QStringLiteral("playlist.mlt")));
    REQUIRE(unsupported.open(QIODevice::WriteOnly));
    REQUIRE(unsupported.write("<mlt/>") == 6);
    unsupported.close();
    QFile invalidPng(directory.filePath(QStringLiteral("broken.png")));
    REQUIRE(invalidPng.open(QIODevice::WriteOnly));
    REQUIRE(invalidPng.write("not a PNG") == 9);
    invalidPng.close();
    const QString smallPng = directory.filePath(QStringLiteral("too-small.png"));
    QImage smallImage(4, 4, QImage::Format_RGB32);
    smallImage.fill(Qt::red);
    REQUIRE(smallImage.save(smallPng, "PNG"));
    McpProject project;
    project.open();
    for (const auto &path : {directory.filePath(QStringLiteral("missing.png")), unsupported.fileName(), invalidPng.fileName(), smallPng, directory.path(),
                             QStringLiteral("relative.png"), QStringLiteral("https://example.invalid/image.png")}) {
        auto arguments = project.expected();
        arguments.insert(QStringLiteral("path"), path);
        project.rejectWithoutChange(QStringLiteral("import_media"), arguments);
    }
    project.rejectWithoutChange(QStringLiteral("insert_media_clip"),
                                project.mediaArgs(QStringLiteral("999999"), project.videoTracks.at(0), 0, 0, 8, QStringLiteral("video")));
}
TEST_CASE("MCP failed decoder imports remain recoverable through undo and redo", "[MCP][MCPMedia]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString corruptPath = corruptMp4Fixture(directory);
    const QString validPath = pngFixture(directory);
    McpProject project;
    project.open();
    const auto initialBinCount = pCore->projectItemModel()->getAllClipIds().size();
    auto arguments = project.expected();
    arguments.insert(QStringLiteral("path"), corruptPath);
    const auto imported = project.success(QStringLiteral("import_media"), arguments);
    const QString badBinId = imported.value(QStringLiteral("binId")).toString();
    REQUIRE_FALSE(badBinId.isEmpty());
    CHECK(project.undoStack->index() == 1);
    const auto failed = project.awaitImportTerminal(badBinId);
    REQUIRE(failed.value(QStringLiteral("status")).toString() == QStringLiteral("failed"));
    CHECK(project.timeline->getClipsCount() == 0);

    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.undoStack->index() == 0);
    CHECK_FALSE(pCore->projectItemModel()->getClipByBinID(badBinId));
    project.success(QStringLiteral("redo"), project.expected());
    CHECK(project.undoStack->index() == 1);
    const auto afterRedo = project.awaitImportTerminal(badBinId);
    REQUIRE(afterRedo.value(QStringLiteral("status")).toString() == QStringLiteral("failed"));
    // Native redo may restore a waiting producer without a load job. It must not
    // keep the MCP mutation gate closed forever or make the failed media usable.
    project.rejectWithoutChange(QStringLiteral("insert_media_clip"), project.mediaArgs(badBinId, project.audioTrack, 0, 0, 1, QStringLiteral("audio")));
    arguments = project.expected();
    arguments.insert(QStringLiteral("path"), validPath);
    const auto validImport = project.success(QStringLiteral("import_media"), arguments);
    const QString validBinId = validImport.value(QStringLiteral("binId")).toString();
    REQUIRE_FALSE(validBinId.isEmpty());
    CHECK(validBinId != badBinId);
    project.awaitMediaReady(validBinId);
    CHECK(project.undoStack->index() == 2);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.undoStack->index() == 1);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.undoStack->index() == 0);
    CHECK(pCore->projectItemModel()->getAllClipIds().size() == initialBinCount);
    CHECK(project.timeline->getClipsCount() == 0);
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("MCP visual AV insertion does not mirror audio", "[MCP][MCPMedia][MCPEffects]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    McpProject project;
    project.open();
    const QString binId = importFixture(project, avFixture(directory));
    const auto source = project.binItem(binId);
    CHECK(source.value(QStringLiteral("mediaType")).toString() == QStringLiteral("av"));
    REQUIRE(source.value(QStringLiteral("hasVideo")).toBool());
    REQUIRE(source.value(QStringLiteral("hasAudio")).toBool());
    const int sourceDuration = source.value(QStringLiteral("durationFrames")).toInt();
    REQUIRE(sourceDuration >= 25);
    const int track = project.videoTracks.at(0);
    const auto inserted = project.success(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, track, 12, 3, 9, QStringLiteral("video")));
    const int clipId = inserted.value(QStringLiteral("clipId")).toInt(-1);
    REQUIRE(project.timeline->isClip(clipId));
    CHECK(project.timeline->getClipState(clipId).first == PlaylistState::VideoOnly);
    CHECK(project.timeline->getClipState(clipId).second == ClipType::AV);
    CHECK(project.timeline->getClipTrackId(clipId) == track);
    CHECK(project.timeline->getClipPosition(clipId) == 12);
    CHECK(project.timeline->getClipPlaytime(clipId) == 9);
    CHECK(project.timeline->getClipInOut(clipId) == std::make_pair(3, 11));
    CHECK(project.timeline->getClipsCount() == 1);
    CHECK_FALSE(project.timeline->isInGroup(clipId));
    CHECK(project.undoStack->index() == 2);
    project.rejectWithoutChange(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, project.audioTrack, 0, 0, 5, QStringLiteral("video")));
    project.rejectWithoutChange(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, track, 30, sourceDuration - 1, 2, QStringLiteral("video")));
    project.success(QStringLiteral("undo"), project.expected());
    CHECK(project.timeline->getClipsCount() == 0);
    CHECK(project.undoStack->index() == 1);
    project.success(QStringLiteral("redo"), project.expected());
    REQUIRE(project.timeline->isClip(clipId));
    CHECK(project.timeline->getClipsCount() == 1);
    CHECK(project.timeline->getClipInOut(clipId) == std::make_pair(3, 11));
    const auto audio = project.success(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, project.audioTrack, 0, 2, 5, QStringLiteral("audio")));
    const int audioId = audio.value(QStringLiteral("clipId")).toInt(-1);
    REQUIRE(project.timeline->isClip(audioId));
    CHECK(project.timeline->getClipState(audioId).first == PlaylistState::AudioOnly);
    CHECK(project.timeline->getClipInOut(audioId) == std::make_pair(2, 6));
    CHECK(project.timeline->getClipsCount() == 2);
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("MCP transforms animate source-relative frames and preserve native undo", "[MCP][MCPEffects]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    McpProject project;
    project.open();
    const QString binId = importFixture(project, avFixture(directory));
    const auto inserted =
        project.success(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, project.videoTracks.at(0), 10, 5, 11, QStringLiteral("video")));
    const int clipId = inserted.value(QStringLiteral("clipId")).toInt(-1);
    const auto stack = project.timeline->getClipEffectStackModel(clipId);
    REQUIRE(stack);
    // Disabled foreign effects are retained; an active foreign visual transform is rejected separately.
    REQUIRE(stack->appendEffect(QStringLiteral("brightness")));
    const auto other = std::dynamic_pointer_cast<EffectItemModel>(stack->getEffectStackRow(stack->effectRow(QStringLiteral("brightness"))));
    REQUIRE(other);
    other->setAssetEnabled(false);
    const int otherId = other->getId();
    const QString otherLevel = other->getParam(QStringLiteral("level"));
    const int baselineUndo = project.undoStack->index();
    const QJsonArray frames{transformFrame(0, 10, 20, 100, 80, 0, -30), transformFrame(10, 110, 120, 200, 160, 1, 30)};
    const auto transformed = project.success(QStringLiteral("set_clip_transform"), transformArgs(project, clipId, frames));
    CHECK(project.undoStack->index() == baselineUndo + 1);
    const auto applied = appliedEffect(transformed, QStringLiteral("transform"));
    CHECK(applied.value(QStringLiteral("effectId")).toString() == QStringLiteral("qtblend"));
    const int transformId = applied.value(QStringLiteral("effectInstanceId")).toInt(-1);
    auto effect = effectInstance(stack, transformId);
    REQUIRE(effect);
    CHECK(applied.value(QStringLiteral("parameters")).toObject().value(QStringLiteral("rect")).toString() == effect->getParam(QStringLiteral("rect")));
    CHECK(applied.value(QStringLiteral("parameters")).toObject().value(QStringLiteral("rotation")).toString() == effect->getParam(QStringLiteral("rotation")));
    CHECK(effect->filter().get_int("distort") == 1);
    requireTransformSample(effect, 5, 10, 20, 100, 80, 0, -30);
    requireTransformSample(effect, 10, 60, 70, 150, 120, 0.5, 0);
    requireTransformSample(effect, 15, 110, 120, 200, 160, 1, 30);
    const auto anchor = effect->filter().anim_get_rect("rotate_anchor", 10);
    CHECK(anchor.x == Approx(0.5));
    CHECK(anchor.y == Approx(0.5));
    REQUIRE(effectInstance(stack, otherId));
    CHECK_FALSE(other->isAssetEnabled());
    CHECK(other->getParam(QStringLiteral("level")) == otherLevel);
    const int rows = stack->rowCount();

    const QJsonArray replacement{transformFrame(0, 20, 30, 80, 60, 0.33, 12.34), transformFrame(10, 200, 130, 180, 160, 0.67, -12.34)};
    const auto replaced = project.success(QStringLiteral("set_clip_transform"), transformArgs(project, clipId, replacement));
    CHECK(appliedEffect(replaced, QStringLiteral("transform")).value(QStringLiteral("effectInstanceId")).toInt(-1) == transformId);
    CHECK(stack->rowCount() == rows);
    CHECK(project.undoStack->index() == baselineUndo + 2);
    requireTransformSample(effect, 5, 20, 30, 80, 60, 0.33, 12.34);
    requireTransformSample(effect, 15, 200, 130, 180, 160, 0.67, -12.34);
    project.success(QStringLiteral("undo"), project.expected());
    requireTransformSample(effect, 5, 10, 20, 100, 80, 0, -30);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK_FALSE(effectInstance(stack, transformId));
    REQUIRE(effectInstance(stack, otherId));
    CHECK_FALSE(other->isAssetEnabled());
    project.success(QStringLiteral("redo"), project.expected());
    REQUIRE(effectInstance(stack, transformId));
    project.success(QStringLiteral("redo"), project.expected());
    requireTransformSample(effectInstance(stack, transformId), 15, 200, 130, 180, 160, 0.67, -12.34);
    CHECK(other->getParam(QStringLiteral("level")) == otherLevel);
    REQUIRE(stack->checkConsistency());
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("MCP audio gain and fade endpoint spans use native effects and undo", "[MCP][MCPEffects]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    McpProject project;
    project.open();
    const QString binId = importFixture(project, wavFixture(directory));
    const auto inserted =
        project.success(QStringLiteral("insert_media_clip"), project.mediaArgs(binId, project.audioTrack, 10, 3, 15, QStringLiteral("audio")));
    const int clipId = inserted.value(QStringLiteral("clipId")).toInt(-1);
    const auto stack = project.timeline->getClipEffectStackModel(clipId);
    REQUIRE(stack);
    const int baselineUndo = project.undoStack->index();
    const auto baselineAudio = renderedAudioRms(project.timeline, 10, 15);
    REQUIRE(baselineAudio.at(6).first > 0.1);
    REQUIRE(baselineAudio.at(6).second > 0.05);
    const auto result = project.success(QStringLiteral("set_clip_audio"), audioArgs(project, clipId, -6, 3, 4));
    REQUIRE(result.value(QStringLiteral("appliedEffects")).toArray().size() == 3);
    CHECK(project.undoStack->index() == baselineUndo + 1);
    const int gainId = appliedEffect(result, QStringLiteral("gain")).value(QStringLiteral("effectInstanceId")).toInt(-1);
    const int inId = appliedEffect(result, QStringLiteral("fadeIn")).value(QStringLiteral("effectInstanceId")).toInt(-1);
    const int outId = appliedEffect(result, QStringLiteral("fadeOut")).value(QStringLiteral("effectInstanceId")).toInt(-1);
    const auto gain = effectInstance(stack, gainId);
    const auto fadeIn = effectInstance(stack, inId);
    const auto fadeOut = effectInstance(stack, outId);
    REQUIRE(gain);
    REQUIRE(fadeIn);
    REQUIRE(fadeOut);
    CHECK(gain->filter().anim_get_double("level", 3) == Approx(-6));
    CHECK(gain->filter().anim_get_double("level", 17) == Approx(-6));
    CHECK(appliedEffect(result, QStringLiteral("gain")).value(QStringLiteral("parameters")).toObject().value(QStringLiteral("level")).toString() ==
          gain->getParam(QStringLiteral("level")));
    CHECK(fadeIn->filter().get_in() == 3);
    CHECK(fadeIn->filter().get_out() == 6);
    CHECK(fadeOut->filter().get_in() == 13);
    CHECK(fadeOut->filter().get_out() == 17);
    CHECK(stack->getFadePosition(true) == 3);
    CHECK(stack->getFadePosition(false) == 4);
    const auto effectedAudio = renderedAudioRms(project.timeline, 10, 15);
    const double amplitude = std::pow(10.0, -6.0 / 20.0);
    CHECK(effectedAudio.at(6).first / baselineAudio.at(6).first == Approx(amplitude).margin(0.002));
    CHECK(effectedAudio.at(6).second / baselineAudio.at(6).second == Approx(amplitude).margin(0.002));
    CHECK(effectedAudio.first().first < 0.00001);
    CHECK(effectedAudio.first().second < 0.00001);
    // The native volume filter ramps per sample from the previous frame's gain.
    // Its final frame therefore has residual energy while ending at silence.
    CHECK(effectedAudio.last().first < baselineAudio.last().first * amplitude * 0.4);
    CHECK(effectedAudio.last().second < baselineAudio.last().second * amplitude * 0.4);
    const int rowsWithFades = stack->rowCount();
    const auto noFades = project.success(QStringLiteral("set_clip_audio"), audioArgs(project, clipId, 0, 0, 0));
    REQUIRE(noFades.value(QStringLiteral("appliedEffects")).toArray().size() == 1);
    CHECK(appliedEffect(noFades, QStringLiteral("gain")).value(QStringLiteral("effectInstanceId")).toInt(-1) == gainId);
    CHECK(stack->rowCount() == rowsWithFades - 2);
    CHECK_FALSE(effectInstance(stack, inId));
    CHECK_FALSE(effectInstance(stack, outId));
    CHECK(gain->filter().anim_get_double("level", 3) == Approx(0));
    CHECK(stack->getFadePosition(true) == 0);
    CHECK(stack->getFadePosition(false) == 0);
    project.success(QStringLiteral("undo"), project.expected());
    REQUIRE(effectInstance(stack, inId));
    REQUIRE(effectInstance(stack, outId));
    CHECK(gain->filter().anim_get_double("level", 3) == Approx(-6));
    CHECK(stack->getFadePosition(true) == 3);
    CHECK(stack->getFadePosition(false) == 4);
    project.success(QStringLiteral("undo"), project.expected());
    CHECK_FALSE(effectInstance(stack, gainId));
    CHECK_FALSE(effectInstance(stack, inId));
    CHECK_FALSE(effectInstance(stack, outId));
    project.success(QStringLiteral("redo"), project.expected());
    REQUIRE(effectInstance(stack, gainId));
    REQUIRE(effectInstance(stack, inId));
    REQUIRE(effectInstance(stack, outId));
    project.success(QStringLiteral("redo"), project.expected());
    CHECK_FALSE(effectInstance(stack, inId));
    CHECK_FALSE(effectInstance(stack, outId));
    CHECK(gain->filter().anim_get_double("level", 3) == Approx(0));
    REQUIRE(stack->checkConsistency());
    REQUIRE(project.timeline->checkConsistency());
}

TEST_CASE("MCP effect controls reject invalid values and incompatible targets without edits", "[MCP][MCPEffects]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    McpProject project;
    project.open();
    const QString imageBin = importFixture(project, pngFixture(directory));
    const QString audioBin = importFixture(project, wavFixture(directory));
    const int videoTrack = project.videoTracks.at(0);
    const auto image = project.success(QStringLiteral("insert_media_clip"), project.mediaArgs(imageBin, videoTrack, 0, 0, 20, QStringLiteral("video")));
    const int imageId = image.value(QStringLiteral("clipId")).toInt(-1);
    const auto audio = project.success(QStringLiteral("insert_media_clip"), project.mediaArgs(audioBin, project.audioTrack, 0, 0, 20, QStringLiteral("audio")));
    const int audioId = audio.value(QStringLiteral("clipId")).toInt(-1);
    const auto first = transformFrame(0, 0, 0, 100, 100, 1, 0);
    const auto last = transformFrame(19, 10, 20, 200, 200, 0.5, 30);
    const QJsonArray frames{first, last};

    SECTION("Bounded transform frames and numeric values")
    {
        for (const QJsonArray &invalid : {QJsonArray{}, QJsonArray{last}, QJsonArray{first, first}, QJsonArray{last, first},
                                          QJsonArray{first, transformFrame(20, 0, 0, 100, 100, 1, 0)}, QJsonArray{transformFrame(0, 0, 0, 0, 100, 1, 0)},
                                          QJsonArray{transformFrame(0, 0, 0, 100, 100, 1.1, 0)}, QJsonArray{transformFrame(0, 0, 0, 100, 100, 1, 361)}}) {
            project.rejectWithoutChange(QStringLiteral("set_clip_transform"), transformArgs(project, imageId, invalid));
        }
        QJsonArray tooMany;
        for (int frame = 0; frame < 65; ++frame)
            tooMany.append(transformFrame(frame, 0, 0, 100, 100, 1, 0));
        project.rejectWithoutChange(QStringLiteral("set_clip_transform"), transformArgs(project, imageId, tooMany));
        auto missing = first;
        missing.remove(QStringLiteral("rotation"));
        project.rejectWithoutChange(QStringLiteral("set_clip_transform"), transformArgs(project, imageId, QJsonArray{missing}));
    }
    SECTION("Fractional bitmap geometry is rejected before native integer quantization")
    {
        for (const auto &field : {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("width"), QStringLiteral("height")}) {
            auto fractional = first;
            fractional.insert(field, first.value(field).toDouble() + 0.5);
            project.rejectWithoutChange(QStringLiteral("set_clip_transform"), transformArgs(project, imageId, QJsonArray{fractional, last}));
        }
    }
    SECTION("Native PNG source ranges start at zero")
    {
        project.rejectWithoutChange(QStringLiteral("insert_media_clip"),
                                    project.mediaArgs(imageBin, project.videoTracks.at(1), 0, 1, 5, QStringLiteral("video")));
    }
    SECTION("Disabled clip effect stacks reject typed commands")
    {
        const auto videoEffects = project.timeline->getClipEffectStackModel(imageId);
        videoEffects->setEffectStackEnabled(false);
        project.rejectWithoutChange(QStringLiteral("set_clip_transform"), transformArgs(project, imageId, frames));
        CHECK_FALSE(videoEffects->isStackEnabled());
        videoEffects->setEffectStackEnabled(true);
        const auto audioEffects = project.timeline->getClipEffectStackModel(audioId);
        audioEffects->setEffectStackEnabled(false);
        project.rejectWithoutChange(QStringLiteral("set_clip_audio"), audioArgs(project, audioId, -3, 2, 2));
        CHECK_FALSE(audioEffects->isStackEnabled());
        audioEffects->setEffectStackEnabled(true);
    }
    SECTION("A user zone on an owned transform is preserved")
    {
        const auto result = project.success(QStringLiteral("set_clip_transform"), transformArgs(project, imageId, frames));
        const auto effects = project.timeline->getClipEffectStackModel(imageId);
        const auto owned = effectInstance(effects, appliedEffect(result, QStringLiteral("transform")).value(QStringLiteral("effectInstanceId")).toInt());
        REQUIRE(owned);
        owned->filter().set_in_and_out(2, 8);
        owned->filter().set("kdenlive:force_in_out", 1);
        const auto before = owned->getParam(QStringLiteral("rect"));
        project.rejectWithoutChange(QStringLiteral("set_clip_transform"), transformArgs(project, imageId, frames));
        CHECK(owned->filter().get_in() == 2);
        CHECK(owned->filter().get_out() == 8);
        CHECK(owned->getParam(QStringLiteral("rect")) == before);
    }
    SECTION("A user zone on an owned gain is preserved")
    {
        const auto result = project.success(QStringLiteral("set_clip_audio"), audioArgs(project, audioId, -3, 0, 0));
        const auto effects = project.timeline->getClipEffectStackModel(audioId);
        const auto owned = effectInstance(effects, appliedEffect(result, QStringLiteral("gain")).value(QStringLiteral("effectInstanceId")).toInt());
        REQUIRE(owned);
        owned->filter().set_in_and_out(1, 8);
        const auto before = owned->getParam(QStringLiteral("level"));
        project.rejectWithoutChange(QStringLiteral("set_clip_audio"), audioArgs(project, audioId, -6, 2, 2));
        CHECK(owned->filter().get_in() == 1);
        CHECK(owned->filter().get_out() == 8);
        CHECK(owned->getParam(QStringLiteral("level")) == before);
    }
    SECTION("Audio parameter bounds and wrong stream kinds")
    {
        project.rejectWithoutChange(QStringLiteral("set_clip_audio"), audioArgs(project, audioId, 13, 0, 0));
        project.rejectWithoutChange(QStringLiteral("set_clip_audio"), audioArgs(project, audioId, -61, 0, 0));
        project.rejectWithoutChange(QStringLiteral("set_clip_audio"), audioArgs(project, audioId, 0, 20, 0));
        project.rejectWithoutChange(QStringLiteral("set_clip_audio"), audioArgs(project, audioId, 0, 10, 10));
        auto fractional = audioArgs(project, audioId, 0, 0, 0);
        fractional.insert(QStringLiteral("fadeInFrames"), 1.5);
        project.rejectWithoutChange(QStringLiteral("set_clip_audio"), fractional);
        project.rejectWithoutChange(QStringLiteral("set_clip_transform"), transformArgs(project, audioId, frames));
        project.rejectWithoutChange(QStringLiteral("set_clip_audio"), audioArgs(project, imageId, 0, 0, 0));
    }
    SECTION("Locked targets and stale state")
    {
        auto stale = transformArgs(project, imageId, frames);
        project.success(QStringLiteral("add_color_clip"), project.colorArgs(project.videoTracks.at(1), 0, 5));
        project.rejectWithoutChange(QStringLiteral("set_clip_transform"), stale);
        project.timeline->setTrackProperty(videoTrack, QStringLiteral("kdenlive:locked_track"), QStringLiteral("1"));
        project.rejectWithoutChange(QStringLiteral("set_clip_transform"), transformArgs(project, imageId, frames));
        project.timeline->setTrackProperty(videoTrack, QStringLiteral("kdenlive:locked_track"), QStringLiteral("0"));
        project.timeline->setTrackProperty(project.audioTrack, QStringLiteral("kdenlive:locked_track"), QStringLiteral("1"));
        project.rejectWithoutChange(QStringLiteral("set_clip_audio"), audioArgs(project, audioId, -3, 2, 2));
        project.timeline->setTrackProperty(project.audioTrack, QStringLiteral("kdenlive:locked_track"), QStringLiteral("0"));
    }
    SECTION("Grouped targets")
    {
        const auto second =
            project.success(QStringLiteral("insert_media_clip"), project.mediaArgs(imageBin, project.videoTracks.at(1), 0, 0, 20, QStringLiteral("video")));
        const int secondId = second.value(QStringLiteral("clipId")).toInt(-1);
        REQUIRE(project.timeline->requestClipsGroup({imageId, secondId}, false) > 0);
        REQUIRE(project.timeline->requestClearSelection());
        REQUIRE(project.timeline->isInGroup(imageId));
        project.rejectWithoutChange(QStringLiteral("set_clip_transform"), transformArgs(project, imageId, frames));
    }
    SECTION("Active foreign visual effects are not silently combined")
    {
        const auto stack = project.timeline->getClipEffectStackModel(imageId);
        REQUIRE(stack->appendEffect(QStringLiteral("brightness")));
        project.rejectWithoutChange(QStringLiteral("set_clip_transform"), transformArgs(project, imageId, frames));
    }
    SECTION("A foreign unique audio fade is not replaced")
    {
        const auto stack = project.timeline->getClipEffectStackModel(audioId);
        REQUIRE(stack->appendEffect(QStringLiteral("fadein")));
        project.rejectWithoutChange(QStringLiteral("set_clip_audio"), audioArgs(project, audioId, -3, 3, 3));
    }
}

#include "mcptimeline_cases.inc"

#include "mcpeffects_cases.inc"

#include "mcplifecycle_cases.inc"

#include "mcpui_cases.inc"

#include "mcpresume_cases.inc"

#include "mcpcancel_cases.inc"

#include "mcpquick_cases.inc"

#include "mcpmedia_cases.inc"

#include "mcpmonitor_cases.inc"

#include "mcpprofiles_cases.inc"

#include "mcpcompositionzone_cases.inc"

#include "mcpduplication_cases.inc"

#include "mcpspeech_cases.inc"

#include "mcpmarkers_cases.inc"

#include "mcptitles_cases.inc"

#include "mcpsequence_state_cases.inc"

#include "mcpstate_cases.inc"

#include "mcpwire_cases.inc"

#include "mcpsubtitle_format_cases.inc"

#include "mcpsubtitle_transaction_cases.inc"

#include "mcpnative_category_cases.inc"

#include "mcp_roto_validation_cases.inc"
